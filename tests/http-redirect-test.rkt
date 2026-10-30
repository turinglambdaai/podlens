#lang racket/base

;; Redirect handling for downloads. The loop variable bug (every hop
;; re-requesting the first URL) shipped in 1.x releases and made every
;; podcast behind an attribution edge unplayable, so this file pins the
;; behavior with a local HTTP server: multi-hop chains land on the final
;; URL, and a 30x that points back at itself is retried, not chased.
;; Run: raco test tests/

(require rackunit
         racket/tcp
         racket/port
         racket/string
         racket/list
         racket/file
         (file "../app/core/http.rkt"))

;; ---- tiny redirect server ---------------------------------------------------
;;
;; Routes (request counts are per full server lifetime):
;;   /chain        → 302 /hop2        → 302 /final        → 200 AUDIO
;;   /self         → 302 /self (twice) → 200 AUDIO         (retry-me edge)
;;   /infinite     → always 302 /infinite                 (budget must stop it)

(define listener #f)
(define server-port #f)
(for ([p (in-range 19443 19454)] #:break listener)
  (with-handlers ([exn:fail? void])
    (set! listener (tcp-listen p 16 #f "127.0.0.1"))
    (set! server-port p)))
(unless listener
  (error 'http-redirect-test "no free port in 19443-19453"))
(define base-url (format "http://127.0.0.1:~a" server-port))
(define audio-bytes (apply bytes (for/list ([i (in-range 1024)]) (modulo i 256))))
(define hits (make-hash))

;; responses are written as raw bytes: round-tripping binary through a
;; latin-1 string would re-encode every high byte as UTF-8 on display
(define (response-for path)
  (define n (hash-ref hits path 0))
  (cond
    [(equal? path "/chain")
     (string->bytes/latin-1
      (string-append "HTTP/1.1 302 Found\r\nLocation: " base-url "/hop2\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"))]
    [(equal? path "/hop2")
     (string->bytes/latin-1
      (string-append "HTTP/1.1 302 Found\r\nLocation: " base-url "/final\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"))]
    [(equal? path "/self")
     (if (< n 2)
         (string->bytes/latin-1
          (string-append "HTTP/1.1 302 Found\r\nLocation: " base-url "/self\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"))
         (bytes-append #"HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nContent-Length: 1024\r\nConnection: close\r\n\r\n"
                       audio-bytes))]
    [(equal? path "/infinite")
     (string->bytes/latin-1
      (string-append "HTTP/1.1 302 Found\r\nLocation: " base-url "/infinite\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"))]
    [else
     (bytes-append #"HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nContent-Length: 1024\r\nConnection: close\r\n\r\n"
                   audio-bytes)]))

(define (serve-once l)
  (define-values (in out) (tcp-accept l))
  (thread
   (lambda ()
     (define buf (open-output-bytes))
     (let loop ()
       (define b (read-byte in))
       (unless (eof-object? b)
         (write-byte b buf)
         (define cur (get-output-bytes buf))
         (define len (bytes-length cur))
         (unless (and (>= len 4) (equal? #"\r\n\r\n" (subbytes cur (- len 4))))
           (loop))))
     (define req (bytes->string/latin-1 (get-output-bytes buf)))
     (define path
       (second (string-split (first (string-split req "\r\n")) " ")))
     (hash-update! hits path add1 0)
     (write-bytes (response-for path) out)
     (flush-output out)
     (close-output-port out)
     (close-input-port in)))
  (serve-once l))

;; serve loop in a background thread; tests shut it down via custodian
(define server-cust (make-custodian))
(parameterize ([current-custodian server-cust])
  ;; the custodian shutdown kills a pending tcp-accept; that exception is
  ;; the normal way this thread ends, so keep it out of the test output
  (thread (lambda () (with-handlers ([exn:fail? void]) (serve-once listener)))))



;; ---- tests -------------------------------------------------------------------

(test-case "multi-hop redirect chain downloads the final resource"
  (define dest (make-temporary-file "pl-redir-~a.bin"))
  (define-values (code written)
    (http-download-file (string-append base-url "/chain") dest))
  (check-equal? code 200)
  (check-equal? written (bytes-length audio-bytes))
  (check-equal? (file->bytes dest) audio-bytes)
  (delete-file dest))

(test-case "a 30x pointing back at itself is retried until it yields"
  (define dest (make-temporary-file "pl-self-~a.bin"))
  (define-values (code written)
    (http-download-file (string-append base-url "/self") dest))
  (check-equal? code 200)
  (check-equal? written (bytes-length audio-bytes))
  (delete-file dest))

(test-case "an endless self-redirect terminates at the hop budget"
  (define dest (make-temporary-file "pl-inf-~a.bin"))
  (define-values (code written)
    (http-download-file (string-append base-url "/infinite") dest))
  ;; bounded work: the loop exhausts its budget and returns the last code
  (check-equal? code 302)
  (check-equal? written 0)
  (delete-file dest))

(with-handlers ([exn:fail? void])
  (custodian-shutdown-all server-cust))
