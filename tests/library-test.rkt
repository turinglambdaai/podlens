#lang racket/base

;; Library store: feed/episode upserts, positions, transcripts — all against
;; a temp PODLENS_DATA_DIR.

(require rackunit
         racket/file
         racket/path
         (file "../app/core/library.rkt"))

(putenv "PODLENS_DATA_DIR" (path->string (make-temporary-file "podlens-test~a" 'directory)))
(library-load!)

(define parsed
  (hasheq 'title "Test Podcast"
          'author "Tester"
          'description "d"
          'artwork-url "https://example.com/art.jpg"
          'items
          (list
           (hasheq 'guid "ep-1" 'title "First"
                   'pub-date-epoch 100 'pub-date-display "x"
                   'enclosure-url "https://example.com/1.mp3"
                   'enclosure-length 1000 'enclosure-type "audio/mpeg"
                   'duration-sec 60 'description "")
           (hasheq 'guid "ep-2" 'title "Second"
                   'pub-date-epoch 200 'pub-date-display "y"
                   'enclosure-url "https://example.com/2.mp3"
                   'enclosure-length 2000 'enclosure-type "audio/mpeg"
                   'duration-sec 90 'description ""))))

(test-case "feed add + episode seeding"
  (define fid (feed-add! "https://example.com/feed.xml" parsed))
  (check-true (string? fid))
  (define eps (episodes-for-feed fid))
  (check-equal? (length eps) 2)
  (check-equal? (hash-ref (car eps) 'title) "Second") ; sorted newest first
  (define fid2 (feed-add! "https://example.com/feed.xml" parsed))
  (check-equal? fid fid2) ; idempotent
  (check-equal? (length (episodes-for-feed fid)) 2))

(test-case "positions and done survive re-add"
  (define fid (feed-add! "https://example.com/feed.xml" parsed))
  (define e (car (episodes-for-feed fid)))
  (define eid (hash-ref e 'id))
  (episode-set-position! eid 42)
  (episode-set-done! eid #t)
  (feed-add! "https://example.com/feed.xml" parsed)
  (check-equal? (hash-ref (episode-get eid) 'position-sec) 42)
  (check-true (hash-ref (episode-get eid) 'done)))

(test-case "transcript files"
  (define fid (feed-add! "https://example.com/feed.xml" parsed))
  (define eid (hash-ref (car (episodes-for-feed fid)) 'id))
  (transcript-save! eid (hasheq 'segments (list (hasheq 'start 0 'end 2 'text "hi"))
                                'translation (list "嗨")
                                'summary #f))
  (define t (transcript-load eid))
  (check-equal? (hash-ref t 'translation) (list "嗨")))

(test-case "feed remove drops episodes"
  (define fid (feed-add! "https://example.com/feed.xml" parsed))
  (check-true (feed-remove! fid))
  (check-false (feed-get fid))
  (check-equal? (length (episodes-for-feed fid)) 0))

(test-case "recent-episodes merges feeds newest first"
  (define fid-a (feed-add! "https://example.com/feed.xml" parsed))
  (define newer
    (hasheq 'title "Newer Show" 'author "T" 'description "d"
            'artwork-url "https://example.com/newer.jpg"
            'items
            (list (hasheq 'guid "ep-n1" 'title "Fresh"
                          'pub-date-epoch 300 'pub-date-display "z"
                          'enclosure-url "https://example.com/n1.mp3"
                          'enclosure-length 1 'enclosure-type "audio/mpeg"
                          'duration-sec 30 'description ""))))
  (define fid-b (feed-add! "https://example.com/newer.xml" newer))
  (define merged (recent-episodes))
  (check-true (>= (length merged) 3))
  (check-equal? (hash-ref (car merged) 'title) "Fresh") ; newest across feeds
  (check-equal? (hash-ref (car merged) 'feed-id) fid-b)
  (check-equal? (hash-ref (cadr merged) 'pub-date-epoch) 200)
  ;; the cap is respected
  (check-equal? (length (recent-episodes 1)) 1))
