#lang racket/base

;; iTunes Search API client — the free, keyless route into Apple's podcast
;; directory. `search` with media=podcast returns authoritative feedUrl for
;; every show, which is what makes a real discovery search possible without
;; running our own directory. Parse-only functions are separate so tests can
;; cover them with fixtures (no network).

(require json
         racket/format
         racket/list
         racket/port
         racket/string
         "http.rkt")

(provide itunes-search-podcasts
         itunes-search-url
         parse-itunes-results
         url-encode-query)

;; Percent-encode for a query component (unreserved characters stay bare;
;; everything else goes out as UTF-8 %XX).
(define url-safe-chars
  "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.~")

(define (encode-byte b)
  (define ch (integer->char b))
  (if (string-contains? url-safe-chars (string ch))
      (string ch)
      (string-upcase (string-append "%" (number->string b 16)))))

(define (url-encode-query s)
  (string-append*
   (for/list ([b (in-bytes (string->bytes/utf-8 s))])
     (encode-byte b))))

(define itunes-search-base "https://itunes.apple.com/search")

(define (itunes-search-url query [limit 25])
  (format "~a?term=~a&media=podcast&entity=podcast&limit=~a"
          itunes-search-base (url-encode-query query) limit))

;; Parsed result row: {id title artist genre feed-url homepage}
;; id is "itunes:<trackId>" — namespaced so it can never collide with a
;; curated catalog id. The wire keys are camelCase (feedUrl etc.); only the
;; parsed row normalizes to kebab-case.
(define (parse-itunes-results jsexpr)
  (for/list ([r (in-list (hash-ref jsexpr 'results '()))]
             #:when (non-empty-string? (hash-ref r 'feedUrl "")))
    (hasheq 'id (format "itunes:~a" (hash-ref r 'trackId 0))
            'title (hash-ref r 'collectionName "")
            'artist (hash-ref r 'artistName "")
            'genre (hash-ref r 'primaryGenreName "")
            'feed-url (hash-ref r 'feedUrl "")
            'homepage (hash-ref r 'collectionViewUrl ""))))

;; Network search. Returns parsed rows; raises on HTTP errors (the caller
;; surfaces them like any other backend error).
(define (itunes-search-podcasts query [limit 25])
  (define-values (code _h body)
    (http-get-bytes (itunes-search-url query limit)))
  (unless (= code 200)
    (error 'itunes-search "iTunes Search returned ~a" code))
  (define parsed (with-input-from-bytes body read-json))
  (parse-itunes-results parsed))
