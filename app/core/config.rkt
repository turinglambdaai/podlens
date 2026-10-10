#lang racket/base

;; User settings. JSON on disk so a user (or an agent driving the CLI) can
;; inspect and edit it directly; every key has a validated default so a
;; half-written file never breaks startup.

(require json
         racket/file
         racket/format
         racket/list
         racket/string
         "paths.rkt"
         "util.rkt")

(provide config-manager?
         make-config-manager
         config-get
         config-set!
         config-reset!
         config-keys
         config-descriptions
         config-api-configured?
         config-bool
         config-number)

;; key -> (list default kind description)
;; kind is one of string secret boolean number enum-lang
(define schema
  (list
   (list 'api-base "https://api.openai.com/v1" 'string
         "OpenAI-compatible API base URL (…/v1)")
   (list 'api-key "" 'secret
         "API key (BYOK; stored locally, never synced)")
   (list 'chat-model "gpt-4o-mini" 'string
         "Chat model for translation and summary")
   (list 'asr-model "whisper-1" 'string
         "Speech-to-text model (provider must expose /audio/transcriptions)")
   (list 'target-lang "zh" 'enum-lang
         "Translation target language (zh / en)")
   (list 'ui-lang "zh" 'enum-lang
         "Interface language for CLI output (zh / en)")
   (list 'asr-chunk-seconds 600 'number
         "Audio chunk length in seconds when ffmpeg splitting is needed")
   (list 'max-transcript-chars 40000 'number
         "Transcript characters sent to the summarizer")
   (list 'check-updates-enabled "true" 'boolean
         "Automatically check for updates (silent, throttled)")
   (list 'last-update-check 0 'number
         "Unix seconds of the last automatic update check (internal)")
   (list 'search-limit 25 'number
         "Max results per discovery search (iTunes Search API)")))

(define known-keys (map car schema))

(define (schema-entry key)
  (findf (lambda (e) (eq? (car e) key)) schema))

(define (default-value key)
  (cadr (schema-entry key)))

(define (config-descriptions)
  (for/list ([e (in-list schema)])
    (list (symbol->string (car e)) (list-ref e 3))))

(struct config-manager (path sem))

(define (make-config-manager [path (config-path)])
  (config-manager path (make-semaphore 1)))

(define (config-keys) (map symbol->string known-keys))

;; ---- validation ----------------------------------------------------------

(define (validate key value)
  (define entry (schema-entry key))
  (unless entry
    (raise-argument-error 'config-set! (format "one of ~a" (string-join (map symbol->string known-keys) ", ")) key))
  (define kind (list-ref entry 2))
  (case kind
    [(string secret)
     (unless (string? value)
       (raise-argument-error 'config-set! "string?" value))
     value]
    [(enum-lang)
     (define v (string-trim (if (string? value) value (format "~a" value))))
     (unless (member v '("zh" "en"))
       (raise-argument-error 'config-set! "\"zh\" or \"en\"" value))
     v]
    [(boolean)
     (define v (string-trim (if (string? value) value (format "~a" value))))
     (cond
       [(member v '("true" "1" "yes")) "true"]
       [(member v '("false" "0" "no")) "false"]
       [else (raise-argument-error 'config-set! "boolean string" value)])]
    [(number)
     (define n (string->number (string-trim (format "~a" value))))
     (unless (and n (exact-positive-integer? n))
       (raise-argument-error 'config-set! "positive integer" value))
     n]
    [else value]))

;; ---- storage ---------------------------------------------------------------

(define (load-hash mgr)
  (with-lock
   (config-manager-sem mgr)
   (lambda ()
     (define v (read-json-file (config-manager-path mgr)))
     (if (hash? v) (make-hash (hash->list v)) (make-hasheq)))))

(define (save-hash! mgr h)
  (with-lock
   (config-manager-sem mgr)
   (lambda ()
     (ensure-data-dir!)
     (write-json-file! (config-manager-path mgr) h))))

(define (config-get mgr key [default-unset #f])
  (define k (if (symbol? key) key (string->symbol key)))
  (define h (load-hash mgr))
  ;; hash-ref calls a procedure failure-result with no args, lazily
  (hash-ref h k (or default-unset (lambda () (default-value k)))))

;; Reads with defaults applied — the only shape pipeline code may rely on.
(define (config-bool mgr key)
  (equal? (config-get mgr key) "true"))

(define (config-number mgr key)
  (define v (config-get mgr key))
  (if (number? v) v (string->number (format "~a" v))))

(define (config-set! mgr key value)
  (define k (if (symbol? key) key (string->symbol key)))
  (define v (validate k value))
  (define h (load-hash mgr))
  (hash-set! h k v)
  (save-hash! mgr h)
  v)

(define (config-reset! mgr key)
  (define k (if (symbol? key) key (string->symbol key)))
  (define h (load-hash mgr))
  (hash-remove! h k)
  (save-hash! mgr h)
  (default-value k))

(define (config-api-configured? mgr)
  (define key (config-get mgr 'api-key))
  (and (string? key) (non-empty-string? (string-trim key))))
