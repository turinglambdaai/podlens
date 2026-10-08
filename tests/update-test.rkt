#lang racket/base

;; Update-channel regression tests. No network, no OpenSSL: the 1.3.0 cycle
;; found that raw-key->pem emitted a stray "#f" into the base64 body (this
;; Racket build's base64-encode appends its second argument verbatim), which
;; made every CLI/backend signature verification fail.

(require rackunit
         net/base64
         racket/list
         racket/string
         (file "../app/update.rkt"))

(define key-raw (base64-decode (string->bytes/latin-1
                                current-update-public-key-hex)))

(test-case "raw-key->pem emits clean SPKI PEM"
  (define pem (raw-key->pem key-raw))
  (define lines (string-split pem "\n"))
  (check-equal? (first lines) "-----BEGIN PUBLIC KEY-----")
  (check-equal? (last lines) "-----END PUBLIC KEY-----")
  ;; the DER body: one or more pure-base64 lines, no stray markers
  (define body (take (drop lines 1) (- (length lines) 2)))
  (check-true (and (pair? body) #t))
  (for ([line (in-list body)])
    (check-regexp-match #px"^[A-Za-z0-9+/]+={0,2}$" line))
  ;; the body must decode back to the 12-byte SPKI prefix + 32-byte key
  (define der (base64-decode (string->bytes/latin-1 (string-append* body))))
  (check-equal? (bytes-length der) 44))

(test-case "update-configured? accepts the embedded key"
  (check-true (update-configured?)))
