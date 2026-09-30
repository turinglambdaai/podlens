#lang racket/base

;; OpenAI-compatible client (BYOK): chat completions for translation and
;; summary, audio transcriptions for ASR. Any provider that speaks the
;; /v1/chat/completions and /v1/audio/transcriptions shapes works
;; (OpenAI, Groq, DeepSeek, SiliconFlow, Ollama, …).

(require json
         net/http-client
         racket/file
         racket/format
         racket/path
         racket/port
         racket/string
         "http.rkt"
         "util.rkt")

(provide chat-completion
         chat-completion-json
         transcribe-file
         api-config-error)

(define (auth-headers cfg)
  (list (format "Authorization: Bearer ~a" (string-trim (hash-ref cfg 'api-key)))))

(define (endpoint cfg tail)
  (string-append (regexp-replace* #rx"/+$" (hash-ref cfg 'api-base) "") tail))

;; Clear, actionable message when the backend is asked to run AI without a
;; usable configuration.
(define (api-config-error cfg)
  (cond
    [(not (hash? cfg)) "no settings loaded"]
    [(let ([b (hash-ref cfg 'api-base "")]) (not (non-empty-string? (string-trim b))))
     "api-base is not configured"]
    [(let ([k (hash-ref cfg 'api-key "")]) (not (non-empty-string? (string-trim k))))
     "api-key is not configured (PodLens is BYOK — set your OpenAI-compatible API key in Settings)"]
    [else #f]))

;; → string content of the first choice
(define (chat-completion cfg system-prompt user-prompt
                         #:json? [json? #f]
                         #:temperature [temperature 0.2])
  (define err (api-config-error cfg))
  (when err (error 'chat-completion err))
  (define body
    (hasheq 'model (hash-ref cfg 'chat-model)
            'temperature temperature
            'messages (list (hasheq 'role "system" 'content system-prompt)
                            (hasheq 'role "user" 'content user-prompt))))
  (define body*
    (if json?
        (hash-set body 'response_format (hasheq 'type "json_object"))
        body))
  (define-values (code _h resp)
    (http-post-json-bytes (endpoint cfg "/chat/completions")
                          (jsexpr->bytes body*)
                          (auth-headers cfg)))
  (when (not (= code 200))
    (raise-api-error 'chat-completion code resp))
  (define parsed (bytes->jsexpr resp))
  (define choices (hash-ref parsed 'choices null))
  (define content-str
    (cond
      [(and (list? choices) (pair? choices))
       (define msg (hash-ref (car choices) 'message (hasheq)))
       (hash-ref msg 'content "")]
      [else ""]))
  (unless (non-empty-string? content-str)
    (error 'chat-completion "provider returned an empty completion"))
  (string-trim content-str))

;; Chat that must return JSON. Strips accidental code fences.
(define (chat-completion-json cfg system-prompt user-prompt #:temperature [temperature 0.2])
  (define raw (chat-completion cfg system-prompt user-prompt
                               #:json? #t
                               #:temperature temperature))
  (define cleaned
    (let* ([s (string-trim raw)]
           [s (regexp-replace* #rx"^```(json)?" s "")]
           [s (regexp-replace* #rx"```$" s "")])
      (string-trim s)))
  (with-handlers ([exn:fail? (lambda (e)
                               (error 'chat-completion-json
                                      "provider did not return valid JSON: ~a"
                                      (exn-message e)))])
    (string->jsexpr cleaned)))

;; Multipart body for the transcription endpoint.
(define (multipart-bytes boundary fields file-field file-name file-bytes)
  (define (part-field name value)
    (string->bytes/utf-8
     (format "--~a\r\nContent-Disposition: form-data; name=\"~a\"\r\n\r\n~a\r\n"
             boundary name value)))
  (define head
    (bytes-append
     (string->bytes/utf-8
      (format "--~a\r\nContent-Disposition: form-data; name=\"~a\"; filename=\"~a\"\r\nContent-Type: application/octet-stream\r\n\r\n"
              boundary file-field file-name))))
  (bytes-append
   (apply bytes-append (map (lambda (kv) (part-field (car kv) (cdr kv))) fields))
   head
   file-bytes
   (string->bytes/utf-8 (format "\r\n--~a--\r\n" boundary))))

;; → jsexpr with keys: text, language, segments ({start end text}, seconds)
(define (transcribe-file cfg audio-path
                         #:model [model #f]
                         #:prompt [prompt #f]
                         #:language [language #f])
  (define err (api-config-error cfg))
  (when err (error 'transcribe-file err))
  (unless (file-exists? audio-path)
    (error 'transcribe-file "audio file not found: ~a" audio-path))
  (define boundary
    (string-append "----podlens"
                   (stable-id (format "~a|~a" (current-inexact-milliseconds) (random 1000000)))))
  (define fields
    (append
     (list (cons "model" (or model (hash-ref cfg 'asr-model)))
           (cons "response_format" "verbose_json"))
     (if prompt (list (cons "prompt" prompt)) '())
     (if language (list (cons "language" language)) '())))
  (define body
    (multipart-bytes boundary fields "file"
                     (path->string (file-name-from-path audio-path))
                     (file->bytes audio-path)))
  (define-values (code _h resp)
    (request-multipart (endpoint cfg "/audio/transcriptions")
                       body
                       boundary
                       (auth-headers cfg)))
  (when (not (= code 200))
    (raise-api-error 'transcribe-file code resp))
  (define parsed (bytes->jsexpr resp))
  (unless (and (hash? parsed) (hash-has-key? parsed 'text))
    (error 'transcribe-file "unexpected transcription response"))
  parsed)

(define (request-multipart url body boundary headers)
  (define-values (ssl? host port path) (split-url url))
  (define conn (http-conn-open host #:ssl? ssl? #:port port))
  (define-values (status resp-headers body-port)
    (http-conn-sendrecv! conn path
                         #:method "POST"
                         #:headers (append headers
                                           (list (format "User-Agent: ~a" http-user-agent)
                                                 (format "Content-Type: multipart/form-data; boundary=~a" boundary)))
                         #:data body))
  (define hs (headers->strings resp-headers))
  (define n (content-length-or-#f hs))
  (define out (if n (read-bytes n body-port) (port->bytes body-port)))
  (close-input-port body-port)
  (values (status-code status) '() out))

(define (raise-api-error who code resp)
  (define msg
    (with-handlers ([exn:fail? (lambda (_) (bytes->string/utf-8 resp))])
      (define parsed (bytes->jsexpr resp))
      (if (and (hash? parsed) (hash-has-key? parsed 'error))
          (let ([e (hash-ref parsed 'error)])
            (if (hash? e)
                (hash-ref e 'message "provider error")
                (format "~a" e)))
          (bytes->string/utf-8 resp))))
  (error who "~a: ~a" code msg))
