#lang racket/base

;; The library: subscriptions + episodes + playback positions, persisted to
;; library.json; transcripts/summaries live in per-episode JSON files.
;;
;; Wire shape notes for the native hosts (everything else is internal):
;;   feed    = {id url title author description artwork-url
;;              added-epoch last-refreshed-epoch}
;;   episode = {id feed-id guid title enclosure-url enclosure-length
;;              enclosure-type pub-date-epoch pub-date-display duration-sec
;;              chapters-url position-sec done downloaded-path downloaded-size
;;              transcript-status summary-status error}
;; transcript file = {episode-id language segments translation target-lang
;;                    summary asr-model chat-model updated-epoch}
;;   segments     = list of {start end text}   (start/end in seconds)
;;   translation  = list of strings, 1:1 with segments (or empty)
;;   summary      = {tldr key-points quotes topics} (or #f)
;; library.json also carries last-episode = {episode-id feed-id} (or absent),
;; the episode the position-saver touched last — what "resume" reopens.

(require json
         racket/file
         racket/list
         racket/path
         racket/set
         racket/string
         "paths.rkt"
         "util.rkt")

(provide library-load!
         library-save!
         library-lock
         feed-add!
         feed-remove!
         feed-all
         feed-get
         feed-unplayed-count
         episode-all
         episodes-for-feed
         episode-get
         episode-set-position!
         episode-set-done!
         episode-set-download!
         episode-set-status!
         last-episode-get
         transcript-load
         transcript-save!
         feed-artwork-of
         episode-sort-key)

;; ---- store ------------------------------------------------------------------

(define lib-sem (make-semaphore 1))
(define lib (make-hasheq '((feeds . ()) (episodes . ()))))

(define (library-lock) lib-sem)

(define (library-load!)
  (with-lock lib-sem
    (lambda ()
      (define v (read-json-file (library-path)))
      (hash-set! lib 'feeds (if (and v (hash? v)) (hash-ref v 'feeds '()) '()))
      (hash-set! lib 'episodes (if (and v (hash? v)) (hash-ref v 'episodes '()) '()))
      (hash-set! lib 'last-episode
                 (if (and v (hash? v)) (hash-ref v 'last-episode #f) #f))
      (void))))

(define (library-save!)
  (with-lock lib-sem
    (lambda () (save-unlocked!))))

;; must be called while holding lib-sem (Racket semaphores are not reentrant)
(define (save-unlocked!)
  (ensure-data-dir!)
  (write-json-file!
   (library-path)
   (hasheq 'feeds (hash-ref lib 'feeds)
           'episodes (hash-ref lib 'episodes)
           'last-episode (hash-ref lib 'last-episode #f))))

;; ---- feeds -------------------------------------------------------------------

(define (feed-all) (hash-ref lib 'feeds))

(define (feed-get id)
  (findf (lambda (f) (equal? (hash-ref f 'id) id)) (feed-all)))

(define (feed-artwork-of feed)
  (hash-ref feed 'artwork-url ""))

;; Insert or update a subscription from a parsed feed (feed.rkt shape).
;; Seeds one episode record per feed item. Returns the feed id.
(define (feed-add! url parsed)
  (with-lock lib-sem
    (lambda ()
      (define id (stable-id url))
      (define existing (findf (lambda (f) (equal? (hash-ref f 'id) id)) (hash-ref lib 'feeds)))
      (define feed
        (hasheq 'id id
                'url url
                'title (hash-ref parsed 'title url)
                'author (hash-ref parsed 'author "")
                'description (hash-ref parsed 'description "")
                'artwork-url (hash-ref parsed 'artwork-url "")
                'added-epoch (or (and existing (hash-ref existing 'added-epoch #f)) (now-epoch))
                'last-refreshed-epoch (now-epoch)))
      (hash-set! lib 'feeds
                 (cons feed (filter (lambda (f) (not (equal? (hash-ref f 'id) id)))
                                    (hash-ref lib 'feeds))))
      ;; upsert episodes; existing rows keep position/status
      (define items
        (filter (lambda (it)
                  (and (non-empty-string? (hash-ref it 'guid ""))
                       (non-empty-string? (hash-ref it 'enclosure-url ""))))
                (hash-ref parsed 'items)))
      (define new-episodes
        (for/list ([it (in-list items)])
          (define eid (episode-id it))
          (define old (findf (lambda (e) (equal? (hash-ref e 'id) eid))
                             (hash-ref lib 'episodes)))
          (if old
              ;; refresh content fields; keep user state (position, done, download, status)
              (let ([e (make-hash (hash->list old))])
                (for ([k '(title enclosure-url enclosure-length enclosure-type
                                pub-date-epoch pub-date-display duration-sec
                                chapters-url)])
                  (hash-set! e k (hash-ref it k #f)))
                (make-immutable-hash (hash->list e)))
              (make-episode id it))))
      ;; this feed's rows are fully replaced by the refreshed set; rows of
      ;; other feeds pass through untouched
      (hash-set! lib 'episodes
                 (append
                  new-episodes
                  (filter (lambda (e)
                            (not (equal? (hash-ref e 'feed-id) id)))
                          (hash-ref lib 'episodes))))
      (save-unlocked!)
      id)))

(define (episode-id item)
  (stable-id (string-append (hash-ref item 'guid) "|" (hash-ref item 'enclosure-url))))

(define (make-episode feed-id item)
  (hasheq 'id (episode-id item)
          'feed-id feed-id
          'guid (hash-ref item 'guid)
          'title (hash-ref item 'title "")
          'enclosure-url (hash-ref item 'enclosure-url)
          'enclosure-length (hash-ref item 'enclosure-length #f)
          'enclosure-type (hash-ref item 'enclosure-type "")
          'pub-date-epoch (hash-ref item 'pub-date-epoch 0)
          'pub-date-display (hash-ref item 'pub-date-display "")
          'duration-sec (hash-ref item 'duration-sec #f)
          'chapters-url (hash-ref item 'chapters-url "")
          'position-sec 0
          'done #f
          'downloaded-path #f
          'downloaded-size #f
          'transcript-status "none"
          'summary-status "none"
          'error ""))

(define (feed-remove! id)
  (with-lock lib-sem
    (lambda ()
      (hash-set! lib 'feeds
                 (filter (lambda (f) (not (equal? (hash-ref f 'id) id))) (hash-ref lib 'feeds)))
      (hash-set! lib 'episodes
                 (filter (lambda (e) (not (equal? (hash-ref e 'feed-id) id))) (hash-ref lib 'episodes)))
      (save-unlocked!)
      #t)))

;; ---- episodes -----------------------------------------------------------------

(define (episode-all) (hash-ref lib 'episodes))

(define (episodes-for-feed feed-id)
  (sort (filter (lambda (e) (equal? (hash-ref e 'feed-id) feed-id)) (episode-all))
        >
        #:key (lambda (e) (hash-ref e 'pub-date-epoch 0))))

;; Episodes not yet marked played — the sidebar badge number.
(define (feed-unplayed-count feed-id)
  (for/sum ([e (in-list (episodes-for-feed feed-id))]
            #:unless (hash-ref e 'done #f))
    1))

(define (episode-get id)
  (findf (lambda (e) (equal? (hash-ref e 'id) id)) (episode-all)))

(define (episode-sort-key e) (hash-ref e 'pub-date-epoch 0))

;; Serialized mutation of a single episode row.
(define (update-episode! id fn)
  (with-lock lib-sem
    (lambda ()
      (define rows (hash-ref lib 'episodes))
      (define hit (findf (lambda (e) (equal? (hash-ref e 'id) id)) rows))
      (define updated
        (and hit
             (let ([e (fn (make-hash (hash->list hit)))])
               (make-immutable-hash (hash->list e)))))
      (when hit
        (hash-set! lib 'episodes
                   (map (lambda (e) (if (equal? (hash-ref e 'id) id) updated e)) rows))
        (save-unlocked!))
      updated)))

(define (episode-set-position! id seconds)
  (update-episode! id (lambda (e) (hash-set! e 'position-sec seconds) e))
  ;; the position saver is the heartbeat of actual listening — whoever
  ;; saved a real position last (not a reset-to-zero) is what "resume" reopens
  (when (> seconds 0)
    (let ([e (episode-get id)])
      (when e
        (with-lock lib-sem
          (lambda ()
            (hash-set! lib 'last-episode
                       (hasheq 'episode-id id 'feed-id (hash-ref e 'feed-id)))
            (save-unlocked!)))))))

(define (last-episode-get) (hash-ref lib 'last-episode #f))

(define (episode-set-done! id done?)
  (update-episode! id (lambda (e) (hash-set! e 'done done?) e)))

(define (episode-set-download! id path size)
  (update-episode! id (lambda (e) (hash-set! e 'downloaded-path path) (hash-set! e 'downloaded-size size) e)))

(define (episode-set-status! id which status [err ""])
  (update-episode! id
                   (lambda (e)
                     (hash-set! e which status)
                     (hash-set! e 'error (if (equal? status "error") err ""))
                     e)))

;; ---- transcripts --------------------------------------------------------------

(define (transcript-load episode-id)
  (read-json-file (transcript-path episode-id)))

(define (transcript-save! episode-id data)
  (define p (transcript-path episode-id))
  (make-directory* (path-only p))
  (write-json-file! p data))
