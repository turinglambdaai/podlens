#lang racket/base

;; iTunes Search parsing (fixtures only — no network in unit tests).
;; Live reachability of itunes.apple.com is exercised by
;; scripts/verify-catalog.rkt-style release checks, not here.

(require rackunit
         (file "../app/core/itunes.rkt"))

(define fixture
  (hasheq 'resultCount 3
          'results
          (list
           (hasheq 'trackId 289429419
                   'collectionName "NPR News Now"
                   'artistName "NPR"
                   'primaryGenreName "News"
                   'feedUrl "https://feeds.npr.org/510318/podcast.xml"
                   'collectionViewUrl "https://podcasts.apple.com/us/podcast/npr-news-now/id289429419"
                   'unrelated 1)
           (hasheq 'trackId 1234
                   'collectionName "Design Details"
                   'artistName "Spec"
                   'primaryGenreName "Design"
                   'feedUrl "https://feeds.example.com/design-details.xml"
                   'collectionViewUrl "https://podcasts.apple.com/x")
           (hasheq 'trackId 5678
                   'collectionName "Broken feed"
                   'artistName "Nobody"
                   'primaryGenreName "Tech"
                   'feedUrl ""))))

(test-case "parse itunes results"
  (define rows (parse-itunes-results fixture))
  ;; the empty feed-url row is dropped
  (check-equal? (length rows) 2)
  (check-equal? (hash-ref (car rows) 'id) "itunes:289429419")
  (check-equal? (hash-ref (car rows) 'title) "NPR News Now")
  (check-equal? (hash-ref (car rows) 'artist) "NPR")
  (check-equal? (hash-ref (car rows) 'genre) "News")
  (check-equal? (hash-ref (car rows) 'feed-url)
                "https://feeds.npr.org/510318/podcast.xml")
  (check-equal? (hash-ref (cadr rows) 'genre) "Design"))

(test-case "empty results"
  (check-equal? (parse-itunes-results (hasheq 'results '())) '()))

(test-case "url encoding"
  (check-equal? (url-encode-query "hello world") "hello%20world")
  (check-equal? (url-encode-query "a&b=c/d") "a%26b%3Dc%2Fd")
  (check-equal? (url-encode-query " untranslated-_.~ ") "%20untranslated-_.~%20")
  ;; non-ASCII encodes as UTF-8
  (check-equal? (url-encode-query "é") "%C3%A9")
  (check-equal? (itunes-search-url "atp" 10)
                "https://itunes.apple.com/search?term=atp&media=podcast&entity=podcast&limit=10"))
