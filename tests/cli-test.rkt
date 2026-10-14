#lang racket/base

;; CLI id handling: every command prints 12-char id prefixes, so the CLI
;; must accept a full id or an unambiguous prefix; ambiguous prefixes fail
;; with a distinct message. Unit tests hit the resolvers, subprocess runs
;; exercise the real dispatch.

(require json
         rackunit
         racket/file
         racket/format
         racket/port
         racket/string
         (file "../app/cli.rkt")
         (file "../app/core/library.rkt"))

(define data-dir (path->string (make-temporary-file "podlens-cli~a" 'directory)))
(putenv "PODLENS_DATA_DIR" data-dir)
(library-load!)

(define (item guid title enclosure)
  (hasheq 'guid guid 'title title
          'pub-date-epoch 100 'pub-date-display "2026-01-01"
          'enclosure-url enclosure
          'enclosure-length 1000 'enclosure-type "audio/mpeg"
          'duration-sec 60 'description ""))

(define parsed-a
  (hasheq 'title "Pod A" 'author "a" 'description "d" 'artwork-url ""
          'items (list (item "ep-1" "First" "https://example.com/1.mp3")
                       (item "ep-2" "Second" "https://example.com/2.mp3"))))
(define parsed-b
  (hasheq 'title "Pod B" 'author "b" 'description "d" 'artwork-url ""
          'items (list (item "ep-3" "Third" "https://example.com/3.mp3"))))

(define fid-a (feed-add! "https://example.com/a.xml" parsed-a))
(feed-add! "https://example.com/b.xml" parsed-b)
(define eid-a (hash-ref (car (episodes-for-feed fid-a)) 'id))

(test-case "resolvers accept exact, prefix, and reject ambiguous or missing"
  (check-equal? (hash-ref (resolve-feed fid-a) 'title) "Pod A")
  (check-equal? (hash-ref (resolve-feed (substring fid-a 0 12)) 'title) "Pod A")
  (check-equal? (hash-ref (resolve-episode eid-a) 'title) "First")
  (check-equal? (hash-ref (resolve-episode (substring eid-a 0 5)) 'title) "First")
  ;; empty prefix matches everything -> ambiguous; unknown -> #f
  (check-eq? (resolve-feed "") 'ambiguous)
  (check-eq? (resolve-episode "") 'ambiguous)
  (check-eq? (resolve-feed "zzzz") #f)
  (check-eq? (resolve-episode "zzzz") #f))

;; ---- real dispatch over subprocess -------------------------------------------

(define here (or (current-load-relative-directory) (current-directory)))
(define cli-path (path->string (simplify-path (build-path here ".." "app" "cli.rkt"))))
(define racket-path (find-executable-path "racket"))

(define (run-cli . args)
  (define envs (environment-variables-copy (current-environment-variables)))
  (environment-variables-set! envs #"PODLENS_DATA_DIR" (string->bytes/utf-8 data-dir))
  (define-values (child stdout stdin stderr)
    (parameterize ([current-environment-variables envs])
      (apply subprocess #f #f #f racket-path cli-path args)))
  (close-output-port stdin)
  (define out (string-trim (port->string stdout)))
  (close-input-port stdout)
  (close-input-port stderr)
  (subprocess-wait child)
  (values (subprocess-status child) out))

(test-case "episodes and episode-id commands accept the shown 12-char prefix"
  (define-values (code1 out1) (run-cli "episodes" (substring fid-a 0 12)))
  (check-equal? code1 0)
  (check-true (string-contains? out1 "First"))
  (define-values (code2 out2) (run-cli "--json" "episodes" (substring fid-a 0 12)))
  (check-equal? code2 0)
  (define js (string->jsexpr out2))
  (check-true (hash-ref js 'ok))
  (check-equal? (length (hash-ref js 'episodes)) 2)
  (define-values (code3 _) (run-cli "done" (substring eid-a 0 12) "1"))
  (check-equal? code3 0)
  (library-load!) ; the change landed on disk in the child process
  (check-true (hash-ref (episode-get eid-a) 'done)))

(test-case "ambiguous and unknown ids fail with the documented exit codes"
  (define-values (amb-code amb-out) (run-cli "--json" "done" ""))
  (check-equal? amb-code 1)
  (check-false (hash-ref (string->jsexpr amb-out) 'ok))
  (define-values (miss-code _) (run-cli "episodes" "zzzz"))
  (check-equal? miss-code 2))
