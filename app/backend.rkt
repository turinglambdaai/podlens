#lang racket/base

;; PodLens backend — the single RVT1 surface shared by the macOS and
;; Windows hosts. Everything declared here is the wire contract: changing
;; an RPC name, an Event, a State, or a row layout is a cross-platform
;; release, not a local edit.
;;
;; Rows are positional strings (Fulcrum style); ids are stable hex strings
;; from app/core/library.rkt.
;;
;;   feed-list row    = (id title author artwork-url episode-count
;;                       latest-title latest-pub)
;;   episode-list row = (id title pub-display duration-sec downloaded
;;                       transcript-status summary-status position-sec done
;;                       has-translation)
;;   transcript row   = (start end text translation)
;;
;; Lifecycle: the embedded runtime calls `start` with the two RVT1 fds.
;; library/config managers are built once here and shared by every request
;; worker; tests drive app/core directly instead.

(require json
         racket/format
         racket/file
         racket/list
         racket/string
         rivet/backend
         "core/config.rkt"
         "core/feed.rkt"
         "core/i18n.rkt"
         "core/library.rkt"
         "core/paths.rkt"
         "core/pipeline.rkt"
         "core/util.rkt"
         (prefix-in upd: "update.rkt"))

(provide start
         app-version)

(define app-version "1.0.0")

;; ---- States / Events ------------------------------------------------------

(define-state version : String app-version)

(define-event notify : String)
(define-event job-progress : (List String))
(define-event episodes-changed : String)
(define-event open-url : String)
(define-event update-available : String)

;; ---- managers --------------------------------------------------------------

(define current-config (make-parameter #f))
(define current-jobs (make-parameter #f))

(define (config!)
  (define c (current-config))
  (unless c (error 'backend "config is not initialized; start was not called"))
  c)

(define (lang!) (config-get (config!) 'ui-lang))

;; openai-ready settings snapshot for the pipeline
(define (cfg!)
  (cfg-snapshot (config!)))

;; ---- feeds -------------------------------------------------------------------

(define-rpc (health : String)
  (format "podlens ~a feeds=~a episodes=~a"
          app-version
          (length (feed-all))
          (length (episode-all))))

(define-rpc (feed-add [url String] : String)
  (define parsed (fetch-feed (string-trim url)))
  (define id (feed-add! (string-trim url) parsed))
  (episodes-changed id)
  id)

(define-rpc (feed-remove [id String] : Bool)
  (and (feed-get id) (feed-remove! id)))

(define-rpc (feed-list : (List (List String)))
  (for/list ([f (in-list (feed-all))])
    (define eps (episodes-for-feed (hash-ref f 'id)))
    (define latest (and (pair? eps) (car eps)))
    (list (hash-ref f 'id)
          (hash-ref f 'title)
          (hash-ref f 'author)
          (hash-ref f 'artwork-url "")
          (format "~a" (length eps))
          (if latest (hash-ref latest 'title "") "")
          (if latest (hash-ref latest 'pub-date-display "") ""))))

(define-rpc (feed-refresh [id String] : Int64)
  (define f (feed-get id))
  (unless f (error 'feed-refresh "no such feed: ~a" id))
  (define before (length (episodes-for-feed id)))
  (define parsed (fetch-feed (hash-ref f 'url)))
  (feed-add! (hash-ref f 'url) parsed)
  (define added (- (length (episodes-for-feed id)) before))
  (episodes-changed id)
  added)

(define-rpc (feed-refresh-all : Int64)
  (for/sum ([f (in-list (feed-all))])
    (feed-refresh (hash-ref f 'id))))

;; ---- episodes -------------------------------------------------------------------

(define-rpc (episode-list [feed-id String] : (List (List String)))
  (for/list ([e (in-list (episodes-for-feed feed-id))])
    (define t (transcript-load (hash-ref e 'id)))
    (list (hash-ref e 'id)
          (hash-ref e 'title)
          (hash-ref e 'pub-date-display)
          (format "~a" (or (hash-ref e 'duration-sec #f) ""))
          (if (hash-ref e 'downloaded-path #f) "1" "0")
          (hash-ref e 'transcript-status "none")
          (hash-ref e 'summary-status "none")
          (format "~a" (hash-ref e 'position-sec 0))
          (if (hash-ref e 'done #f) "1" "0")
          (if (and t (pair? (hash-ref t 'translation '()))) "1" "0"))))

(define-rpc (episode-download [id String] : String)
  (define e (episode-get id))
  (unless e (error 'episode-download "no such episode: ~a" id))
  (job-start! (current-jobs) "download" id
              (lambda (progress)
                (ensure-audio! e (lambda (d t) (progress 50 "downloading"))))))

(define-rpc (episode-transcribe [id String] : String)
  (define e (episode-get id))
  (unless e (error 'episode-transcribe "no such episode: ~a" id))
  (job-start! (current-jobs) "transcribe" id
              (lambda (progress)
                (define n (run-transcribe! (cfg!) e
                                           (lambda (p m) (progress p m))))
                (notify (tr (lang!) 'transcribed n)))))

(define-rpc (episode-translate [id String] : String)
  (define e (episode-get id))
  (unless e (error 'episode-translate "no such episode: ~a" id))
  (job-start! (current-jobs) "translate" id
              (lambda (progress)
                (define n (run-translate! (cfg!) e
                                          (lambda (p m) (progress p m))))
                (notify (tr (lang!) 'translated n)))))

(define-rpc (episode-summarize [id String] : String)
  (define e (episode-get id))
  (unless e (error 'episode-summarize "no such episode: ~a" id))
  (job-start! (current-jobs) "summarize" id
              (lambda (progress)
                (run-summarize! (cfg!) e (lambda (p m) (progress p m)))
                (notify (tr (lang!) 'summarized)))))

(define-rpc (episode-remove-audio [id String] : Bool)
  (define e (episode-get id))
  (and e
       (let ([p (hash-ref e 'downloaded-path #f)])
         (when (and p (file-exists? p))
           (with-handlers ([exn:fail? void]) (delete-file p)))
         (episode-set-download! id #f #f)
         #t)))

(define-rpc (episode-transcript [id String] : (List (List String)))
  (define t (transcript-load id))
  (if t
      (let ([segs (hash-ref t 'segments '())]
            [trans (hash-ref t 'translation '())])
        (for/list ([s (in-list segs)] [i (in-naturals)])
          (list (format "~a" (hash-ref s 'start 0))
                (format "~a" (hash-ref s 'end 0))
                (hash-ref s 'text "")
                (if (< i (length trans)) (list-ref trans i) ""))))
      '()))

(define-rpc (episode-summary [id String] : String)
  (define t (transcript-load id))
  (define s (and t (hash-ref t 'summary #f)))
  (if s
      (jsexpr->string s)
      ""))

(define-rpc (position-save [id String] [seconds String] [done String] : Bool)
  (and (episode-get id)
       (begin
         (episode-set-position! id (or (string->number seconds) 0))
         (episode-set-done! id (member (string-trim done) '("1" "true")))
         #t)))

;; ---- jobs -----------------------------------------------------------------------

(define-rpc (job-status [job-id String] : String)
  (define j (job-get (current-jobs) job-id))
  (if j (jsexpr->string j) ""))

;; ---- settings ----------------------------------------------------------------------

(define-rpc (settings-list : (List (List String)))
  (define c (config!))
  (for/list ([key (in-list (config-keys))])
    (list key
          (format "~a" (config-get c key))
          (for/or ([d (in-list (config-descriptions))]
                   #:when (equal? (car d) key))
            (cadr d)))))

(define-rpc (settings-set [key String] [value String] : Bool)
  (define c (config!))
  (unless (member key (config-keys))
    (error 'settings-set "unknown setting: ~a" key))
  (config-set! c key value)
  #t)

;; ---- updates ---------------------------------------------------------------------------

(define-rpc (update-check : String)
  (define status
    (if (upd:update-configured?)
        (upd:update-check app-version)
        (tr (lang!) 'update-dev)))
  (when (string-prefix? status "update available")
    (update-available status))
  status)

(define-rpc (open-releases : Void)
  (open-url upd:releases-page)
  (void))

;; ---- lifecycle --------------------------------------------------------------------------

(define (start in-fd out-fd)
  (ensure-data-dir!)
  (define mgr (make-config-manager))
  (define jobs (make-job-manager
                (lambda (job-id kind pct message)
                  (job-progress (list job-id kind (~a pct) message)))))
  (library-load!)
  (current-config mgr)
  (current-jobs jobs)
  (state-set! version app-version)
  (serve-fds in-fd out-fd))
