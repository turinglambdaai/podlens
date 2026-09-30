#lang racket/base

;; Verify every feed in app/core/catalog.rkt against the live wire using
;; the app's own HTTP client and RSS parser. A catalog entry only earns its
;; place if it parses right now — no hand-copied URLs that died in 2021.
;;
;; Run: racket scripts/verify-catalog.rkt [--json]
;; Exit 0 when every entry verifies; exit 1 otherwise.

(require json
         racket/format
         racket/list
         racket/match
         racket/port
         racket/string
         (file "../app/core/feed.rkt")
         (file "../app/core/catalog.rkt"))

(define entries (catalog-entries))

(define results
  (for/list ([e (in-list entries)])
    (define url (catalog-entry-url e))
    (define started (current-inexact-milliseconds))
    (define outcome
      (with-handlers ([exn:fail? (lambda (err) (exn-message err))])
        (define parsed (fetch-feed url))
        (define title (string-trim (hash-ref parsed 'title "")))
        (define n (length (hash-ref parsed 'items '())))
        (if (and (non-empty-string? title) (>= n 1))
            (format "ok: ~a (~a episodes)" title n)
            "parsed but empty")))
    (define ms (inexact->exact (floor (- (current-inexact-milliseconds) started))))
    (list (catalog-entry-id e) url outcome ms)))

(for ([r (in-list results)])
  (match-define (list id url outcome ms) r)
  (displayln (format "[~a] ~a\n    ~a — ~a (~ams)" id url outcome (~a ms) ms)))

(define failed (filter (lambda (r) (not (string-prefix? (list-ref r 2) "ok:"))) results))
(when (pair? failed)
  (eprintf "\n~a/~a catalog entries failed verification\n" (length failed) (length results)))
(exit (if (null? failed) 0 1))
