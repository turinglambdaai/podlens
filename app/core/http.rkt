#lang racket/base

;; Minimal HTTP(S) client over net/http-client: byte GETs, JSON POSTs,
;; streamed downloads with progress, and multipart uploads (ASR). Redirects
;; are followed manually so every hop keeps the User-Agent the feeds/CDNs
;; and the GitHub API require.

(require net/http-client
         json
         racket/file
         racket/format
         racket/list
         racket/match
         racket/port
         racket/string)

(provide http-get-bytes
         http-get-string
         http-post-json-bytes
         http-download-file
         split-url
         http-user-agent
         headers->strings
         content-length-or-#f
         status-code)

(define http-user-agent "PodLens/1.0 (+https://github.com/turinglambdaai/podlens)")

;; ---- URL splitting (no net/url dependency; feeds are plain http(s)) ------

;; → (values ssl? host port path)
(define (split-url u)
  (define m
    (regexp-match #rx"^https?://([^/:?#]+)(:([0-9]+))?(/[^?#]*)?(\\?.*)?$" u))
  (unless m
    (raise-argument-error 'split-url "absolute http(s) URL" u))
  (define ssl? (string-prefix? u "https://"))
  (define host (list-ref m 1))
  (define port
    (cond
      [(list-ref m 3) (string->number (list-ref m 3))]
      [ssl? 443]
      [else 80]))
  (define path
    (string-append
     (or (list-ref m 4) "/")
     (or (list-ref m 5) "")))
  (values ssl? host port path))

;; ---- request core ----------------------------------------------------------

;; header lines arrive as byte strings; normalize once
(define (headers->strings headers)
  (map (lambda (h) (if (bytes? h) (bytes->string/utf-8 h) h)) headers))

(define (header-value headers name)
  (define m
    (findf (lambda (h)
             (regexp-match (format "(?i:^~a:)" (regexp-quote name)) h))
           headers))
  (and m
       (string-trim
        (second (regexp-match (format "(?i:^~a:\\s*(.*)$)" (regexp-quote name)) m)))))

(define (content-length-or-#f headers)
  (define v (header-value headers "content-length"))
  (and v (string->number (string-trim v))))

;; One request, no redirects. → (values status headers body-bytes)
(define (request-once method url-string headers [data #f])
  (define-values (ssl? host port path) (split-url url-string))
  (define conn
    (http-conn-open host #:ssl? ssl? #:port port))
  (define all-headers
    (append (list (format "User-Agent: ~a" http-user-agent)
                  "Accept: */*")
            headers))
  (define-values (status resp-headers body-port)
    (if data
        (http-conn-sendrecv! conn path
                             #:method method
                             #:headers all-headers
                             #:data data)
        (http-conn-sendrecv! conn path
                             #:method method
                             #:headers all-headers)))
  (define resp-headers* (headers->strings resp-headers))
  (define n (content-length-or-#f resp-headers*))
  (define body
    (if n
        (read-bytes n body-port)
        (port->bytes body-port)))
  (close-input-port body-port)
  (with-handlers ([exn:fail? void]) (http-conn-close! conn))
  (values (status-code status) resp-headers* body))

;; status line like "HTTP/1.1 200 OK" → 200
(define (status-code status)
  (define s (if (bytes? status) (bytes->string/utf-8 status) status))
  (define m (regexp-match #px"HTTP/[0-9.]+ +([0-9]+)" s))
  (and m (string->number (second m))))

;; GET with up to 5 redirects. → (values code headers body-bytes)
(define (http-get-bytes url-string [extra-headers '()])
  (let loop ([url url-string] [hops 0])
    (define-values (code headers body)
      (request-once "GET" url extra-headers))
    (if (and (member code '(301 302 303 307 308)) (< hops 5))
        (let ([location (header-value headers "location")])
          (unless location
            (error 'http-get-bytes "redirect without Location from ~a" url))
          (loop location (add1 hops)))
        (values code headers body))))

(define (http-get-string url-string [extra-headers '()])
  (define-values (code headers body) (http-get-bytes url-string extra-headers))
  (values code headers (bytes->string/utf-8 body)))

;; POST a JSON body. → (values status headers body-bytes)
(define (http-post-json-bytes url-string json-bytes headers)
  (request-once "POST" url-string
                (append (list "Content-Type: application/json") headers)
                json-bytes))

;; Stream a download to dest-path. on-progress is called as
;; (on-progress done-bytes total-bytes-or-#f) at most every 256 KiB.
;; Returns (values code bytes-written). Raises on network failure only;
;; non-200 responses still write nothing and return the code.
(define (http-download-file url-string dest-path [on-progress (lambda (_a _b) (void))])
  (define-values (ssl? host port path) (split-url url-string))
  (define conn (http-conn-open host #:ssl? ssl? #:port port))
  (define-values (status resp-headers body-port)
    (http-conn-sendrecv! conn path
                         #:method "GET"
                         #:headers (list (format "User-Agent: ~a" http-user-agent)
                                         "Accept: */*")))
  (define code (status-code status))
  (define total (content-length-or-#f (headers->strings resp-headers)))
  (define written 0)
  (define last-reported 0)
  (if (not (= code 200))
      (begin (close-input-port body-port)
             (with-handlers ([exn:fail? void]) (http-conn-close! conn))
             (values code 0))
      (begin
        (with-output-to-file dest-path
          (lambda ()
            (let copy ([chunk (read-bytes 262144 body-port)])
              (unless (eof-object? chunk)
                (write-bytes chunk)
                (set! written (+ written (bytes-length chunk)))
                (when (or (>= (- written last-reported) 262144)
                          (and total (>= written total)))
                  (set! last-reported written)
                  (on-progress written total))
                (copy (read-bytes 262144 body-port)))))
          #:exists 'truncate)
        (close-input-port body-port)
        (with-handlers ([exn:fail? void]) (http-conn-close! conn))
        (values code written))))
