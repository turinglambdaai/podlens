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
    "<?xml version=\"1.0\"?>\n<rss xmlns:itunes=\"http://www.itunes.com/dtds/podcast-1.0.dtd\" version=\"2.0\">\n<channel>\n"
    "<title>Fixture Feed</title><description>test</description><itunes:author>Tester</itunes:author>\n"
    "<item><title>Episode One</title><guid>f-1</guid><pubDate>Mon, 03 Jul 2023 08:00:00 +0000</pubDate>"
    "<itunes:duration>60</itunes:duration>"
    "<enclosure url=\"http://127.0.0.1:0/audio.mp3\" length=\"2048\" type=\"audio/mpeg\"/></item>\n"
    "</channel></rss>")))

;; real container-ish bytes; small enough to skip ffmpeg chunking
(define audio-body (make-bytes 2048 65))

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

;; raco test waits for all threads; the fake server must not outlive the test
(kill-thread server-thread)
