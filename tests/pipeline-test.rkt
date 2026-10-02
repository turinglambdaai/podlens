#lang racket/base

;; End-to-end pipeline test against a local fake OpenAI-compatible server:
;; feed fetch over HTTP, episode download, ASR transcription, translation
;; batching and summarization — all real HTTP, no network.

(require json
         rackunit
         racket/file
         racket/list
         racket/port
         racket/string
         racket/tcp
         (file "../app/core/chapters.rkt")
         (file "../app/core/config.rkt")
         (file "../app/core/feed.rkt")
         (file "../app/core/http.rkt")
         (file "../app/core/library.rkt")
         (file "../app/core/pipeline.rkt"))

(putenv "PODLENS_DATA_DIR" (path->string (make-temporary-file "podlens-test~a" 'directory)))
(library-load!)

;; ---- fake server ---------------------------------------------------------

(define rss-body
  (string->bytes/utf-8
   (string-append
    "<?xml version=\"1.0\"?>\n<rss xmlns:itunes=\"http://www.itunes.com/dtds/podcast-1.0.dtd\" xmlns:podcast=\"https://podcastindex.org/namespace/1.0\" version=\"2.0\">\n<channel>\n"
    "<title>Fixture Feed</title><description>test</description><itunes:author>Tester</itunes:author>\n"
    "<item><title>Episode One</title><guid>f-1</guid><pubDate>Mon, 03 Jul 2023 08:00:00 +0000</pubDate>"
    "<itunes:duration>60</itunes:duration>"
    "<enclosure url=\"http://127.0.0.1:0/audio.mp3\" length=\"2048\" type=\"audio/mpeg\"/></item>\n"
    "<item><title>Episode Two</title><guid>f-2</guid><pubDate>Mon, 05 Jun 2023 08:00:00 +0000</pubDate>"
    "<itunes:duration>90</itunes:duration>"
    "<enclosure url=\"http://127.0.0.1:0/audio.mp3\" length=\"2048\" type=\"audio/mpeg\"/>"
    "<podcast:chapters url=\"http://127.0.0.1:0/chapters.json\" type=\"application/json+chapters\"/></item>\n"
    "</channel></rss>")))

;; real container-ish bytes; small enough to skip ffmpeg chunking
(define audio-body (make-bytes 2048 65))

(define chapters-body
  (jsexpr->bytes
   (hasheq 'version "1"
           'chapters (list (hasheq 'startTime 0 'title "Intro")
                           (hasheq 'startTime 30 'title "Deep dive")
                           ;; shipped as milliseconds by some producers
                           (hasheq 'startTime 90000 'title "Sponsors")))))

(define (chat-response body-bytes)
  (define req (with-input-from-bytes body-bytes (lambda () (read-json))))
  (define system-content
    (hash-ref (first (hash-ref req 'messages)) 'content ""))
  (define user-content
    (hash-ref (last (hash-ref req 'messages)) 'content ""))
  (define content
    (cond
      [(string-contains? system-content "Translate")
       (define lines
         (for/list ([line (in-list (string-split user-content "\n"))]
                    #:when (regexp-match #px"^[0-9]+\\." line))
           (string-append "译:" line)))
       (jsexpr->string (hasheq 'lines lines))]
      [else
       (jsexpr->string (hasheq 'tldr "一句总结。"
                               'key_points (list "要点一" "要点二")
                               'quotes (list (hasheq 'text "quote" 'translation "引用"))
                               'topics (list "测试")))]))
  (jsexpr->bytes
   (hasheq 'choices (list (hasheq 'message (hasheq 'role "assistant" 'content content))))))

(define (asr-response _body)
  (jsexpr->bytes
   (hasheq 'text "hello world"
           'language "en"
           'segments (list (hasheq 'id 0 'start 0.0 'end 1.5 'text "Hello world.")
                           (hasheq 'id 1 'start 1.5 'end 3.0 'text "This is a test.")))))

(define (http-ok body)
  (bytes-append
   (string->bytes/utf-8
    (format "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ~a\r\nConnection: close\r\n\r\n"
            (bytes-length body)))
   body))

(define (handle in out)
  (with-handlers ([exn:fail? void])
    (define request-line (read-line in 'return-linefeed))
    (define path
      (and (string? request-line)
           (third (regexp-match #px"^([A-Z]+) ([^ ]+) HTTP" request-line))))
    (define content-length
      ;; headers end at the first blank line
      (let loop ([n 0])
        (define line (read-line in 'return-linefeed))
        (cond
          [(eof-object? line) n]
          [(equal? line "") n]
          [else
           (define m (regexp-match #px"(?i:content-length:) *([0-9]+)" line))
           (loop (if m (string->number (second m)) n))])))
    (define body (if (and content-length (> content-length 0))
                     (read-bytes content-length in)
                     #""))
    (define response
      (cond
        [(equal? path "/feed.xml") (http-ok rss-body)]
        [(equal? path "/audio.mp3") (http-ok audio-body)]
        [(equal? path "/chapters.json") (http-ok chapters-body)]
        [(equal? path "/v1/audio/transcriptions") (http-ok (asr-response body))]
        [(equal? path "/v1/chat/completions") (http-ok (chat-response body))]
        [else
         (http-ok #"{\"error\":{\"message\":\"not found\"}}")]))
    (write-bytes response out)
    (close-output-port out)
    (close-input-port in)))

;; racket/tcp has no listener-port accessor; probe for a free port instead
(define listener #f)
(define port #f)
(for ([p (in-range 18432 18532)] #:unless port)
  (with-handlers ([exn:fail? void])
    (set! listener (tcp-listen p 16 #t "127.0.0.1"))
    (set! port p)))
(define server-thread
  (thread
   (lambda ()
     (let loop ()
       (define-values (in out) (tcp-accept listener))
       (thread (lambda () (handle in out)))
       (loop)))))

;; rewrite the enclosure URL with the actual port
(set! rss-body
      (string->bytes/utf-8
       (string-replace (bytes->string/utf-8 rss-body) "http://127.0.0.1:0/audio.mp3"
                       (format "http://127.0.0.1:~a/audio.mp3" port))))
(set! rss-body
      (string->bytes/utf-8
       (string-replace (bytes->string/utf-8 rss-body) "http://127.0.0.1:0/chapters.json"
                       (format "http://127.0.0.1:~a/chapters.json" port))))
(set! rss-body
      (string->bytes/utf-8
       (string-replace (bytes->string/utf-8 rss-body) "127.0.0.1:0/feed.xml"
                       (format "127.0.0.1:~a/feed.xml" port))))

;; ---- the test ----------------------------------------------------------------

(define mgr (make-config-manager))
(config-set! mgr 'api-base (format "http://127.0.0.1:~a/v1" port))
(config-set! mgr 'api-key "test-key")
(config-set! mgr 'target-lang "zh")

(define cfg (cfg-snapshot mgr))

(define parsed (fetch-feed (format "http://127.0.0.1:~a/feed.xml" port)))
(define feed-id (feed-add! (format "http://127.0.0.1:~a/feed.xml" port) parsed))
(define episode (car (episodes-for-feed feed-id)))
(define eid (hash-ref episode 'id))

(test-case "transcribe downloads + runs ASR"
  (define n
    (run-transcribe! cfg episode (lambda (p m) (void))))
  (check-equal? n 2)
  (define t (transcript-load eid))
  (check-equal? (hash-ref t 'language) "en")
  (check-equal? (length (hash-ref t 'segments)) 2)
  (check-equal? (hash-ref (second (hash-ref t 'segments)) 'text) "This is a test.")
  (check-true (file-exists? (hash-ref (episode-get eid) 'downloaded-path #f))))

(test-case "translate produces aligned lines"
  (define n (run-translate! cfg (episode-get eid) (lambda (p m) (void))))
  (check-equal? n 2)
  (define t (transcript-load eid))
  (check-equal? (length (hash-ref t 'translation)) 2)
  (check-true (string-prefix? (first (hash-ref t 'translation)) "译:")))

(test-case "summarize stores structured summary"
  (check-true (run-summarize! cfg (episode-get eid) (lambda (p m) (void))))
  (define t (transcript-load eid))
  (check-equal? (hash-ref (hash-ref t 'summary) 'tldr) "一句总结。"))

(define episode-two (second (episodes-for-feed feed-id)))

(test-case "pipeline runs all stages on a fresh episode"
  (define stages
    (run-pipeline! cfg episode-two (lambda (p m) (void))))
  (check-equal? stages '(transcribe translate summarize))
  (define t (transcript-load (hash-ref episode-two 'id)))
  (check-equal? (length (hash-ref t 'translation)) 2)
  (check-equal? (hash-ref (hash-ref t 'summary) 'tldr) "一句总结。"))

(test-case "pipeline skips finished stages"
  (define stages
    (run-pipeline! cfg (episode-get (hash-ref episode-two 'id)) (lambda (p m) (void))))
  (check-equal? stages '()))

(test-case "estimate reflects finished work"
  (define est (estimate-episode cfg (episode-get eid)))
  (check-true (hash-ref est 'transcriptKnown))
  (check-true (hash-ref est 'summarized))
  (check-equal? (hash-ref est 'sentences) 2)
  (check-equal? (hash-ref est 'chars) 27) ; "Hello world." + "This is a test."
  (check-equal? (hash-ref est 'durationSec) 60))

(test-case "chapters load, parse ms timestamps and cache"
  (define marks (chapters-load! (hash-ref episode-two 'id)))
  (check-equal? (length marks) 3)
  (check-equal? (hash-ref (first marks) 'title) "Intro")
  (check-equal? (hash-ref (third marks) 'start) 90.0)
  ;; second load is served from the on-disk cache
  (check-equal? (length (chapters-load! (hash-ref episode-two 'id))) 3))

(test-case "episode without a chapters pointer has none"
  (check-equal? (chapters-load! eid) '()))

(test-case "position save tracks the last-played episode"
  (episode-set-position! eid 42)
  (define last (last-episode-get))
  (check-equal? (hash-ref last 'episode-id) eid)
  (check-equal? (hash-ref last 'feed-id) feed-id))

(test-case "mark unplayed clears the done flag"
  (episode-set-done! eid #t)
  (check-true (hash-ref (episode-get eid) 'done))
  (episode-set-done! eid #f)
  (check-false (hash-ref (episode-get eid) 'done)))

(test-case "unplayed count per feed"
  (episode-set-done! (hash-ref episode-two 'id) #t)
  (check-equal? (feed-unplayed-count feed-id) 1))

;; raco test waits for all threads; the fake server must not outlive the test
(kill-thread server-thread)
