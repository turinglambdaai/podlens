#lang racket/base

;; Chapter marks (Podcasting 2.0 `<podcast:chapters url=…>` pointers).
;; The chapters JSON lives outside the feed; we fetch it lazily, cache it
;; under chapters/<id>.json, and hand the player a sorted list of marks.
;; A missing/invalid payload is not an error — an episode simply has no
;; chapters, and nothing in the UI depends on them.

(require json
         racket/file
         racket/list
         racket/path
         racket/port
         racket/string
         "http.rkt"
         "library.rkt"
         "paths.rkt"
         "util.rkt")

(provide chapters-parse
         chapters-load!)
;; Podcasting 2.0 chapter startTime is seconds; some producers ship
;; milliseconds. Treat values that would place a chapter past a day as ms.
;; Returns #f for unparsable input.
(define (chapter-start v)
  (define n
    (cond
      [(number? v) (exact->inexact v)]
      [(string? v) (string->number (string-trim v))]
      [else #f]))
  (cond
    [(not n) #f]
    [(>= n 86400) (/ n 1000.0)]
    [else n]))

;; jsexpr → list of {start title}, sorted by start; drops marks with
;; unparsable starts.
(define (chapters-parse v)
  (define raw (and (hash? v) (hash-ref v 'chapters #f)))
  (if (list? raw)
      (sort
       (for/list ([c (in-list raw)]
                  #:when (and (hash? c) (chapter-start (hash-ref c 'startTime #f))))
         (hasheq 'start (chapter-start (hash-ref c 'startTime #f))
                 'title (format "~a" (hash-ref c 'title ""))))
       <
       #:key (lambda (c) (hash-ref c 'start)))
      '()))

;; Returns the cached list when present, else fetches the episode's
;; chapters JSON once and caches it. No url / any failure → '().
(define (chapters-load! episode-id)
  (or (let ([cached (read-json-file (chapters-path episode-id))])
        (and cached (chapters-parse cached)))
      (let ([e (episode-get episode-id)])
        (define url (and e (hash-ref e 'chapters-url "")))
        (if (blank-string? url)
            '()
            (with-handlers ([exn:fail? (lambda (_) '())])
              (define-values (code _headers body) (http-get-bytes url))
              (unless (= code 200)
                (error 'chapters-load! "chapters request failed (~a)" code))
              (define v (with-input-from-bytes body read-json))
              (define marks (chapters-parse v))
              (when (pair? marks)
                (let ([p (chapters-path episode-id)])
                  (make-directory* (path-only p))
                  (write-json-file! p v)))
              marks)))))
