#lang racket/base

(require json rackunit racket/file racket/list racket/match racket/path
         racket/string racket/tcp
         rivet/backend rivet/protocol)

(define scratch (make-temporary-file "podlens-wire-~a" 'directory))
(putenv "PODLENS_DATA_DIR" (path->string scratch))
(define test-directory
  (or (current-load-relative-directory) (current-directory)))
(define backend-path
  (simplify-path (build-path test-directory ".." "app" "backend.rkt")))
(void (dynamic-require backend-path #f))
;; `start` normally owns this initialization before it hands file descriptors
;; to serve-fds. The in-memory wire harness uses ports, so reproduce that
;; lifecycle prefix inside the production module's namespace and then call
;; `serve` below.
(parameterize ([current-namespace (module->namespace backend-path)])
  (eval
   '(begin
      (ensure-data-dir!)
      (current-config (make-config-manager))
      (current-jobs
       (make-job-manager (lambda (_job-id _kind _pct _message) (void))))
      (library-load!)
      (state-set! version app-version))))

(define-values (server-in client-out) (make-pipe))
(define-values (client-in server-out) (make-pipe))
(define server-thread (thread (lambda () (serve server-in server-out))))

(define (read-frame/timeout label)
  (define result (box #f))
  (define failure (box #f))
  (define reader
    (thread
     (lambda ()
       (with-handlers ([exn:fail? (lambda (exception)
                                    (set-box! failure exception))])
         (set-box! result (read-frame client-in))))))
  (unless (sync/timeout 30 reader)
    (fail (format "timed out waiting for ~a" label)))
  (when (unbox failure) (raise (unbox failure)))
  (unbox result))

(define next-id 0)
(define (call-rpc name . arguments)
  (set! next-id (add1 next-id))
  (define id next-id)
  (write-frame (frame message:request id (encode-value (cons name arguments)))
               client-out)
  (flush-output client-out)
  (let loop ()
    (define response (read-frame/timeout name))
    (match (frame-type response)
      [(== message:event) (loop)]
      [(== message:response)
       (check-equal? (frame-id response) id)
       (values (decode-value (frame-payload response)) #f)]
      [(== message:error)
       (check-equal? (frame-id response) id)
       (values #f (decode-value (frame-payload response)))]
      [_ (loop)])))

;; ---- fake feed server -------------------------------------------------------
;; Serves one RSS feed with two episodes and their audio, so feed-peek and
;; preview-audio run over real HTTP with no network. racket/tcp has no
;; listener-port accessor; probe for a free port instead.

(define fake-listener #f)
(define fake-port #f)
(for ([p (in-range 18632 18692)] #:unless fake-port)
  (with-handlers ([exn:fail? void])
    (set! fake-listener (tcp-listen p 4 #t "127.0.0.1"))
    (set! fake-port p)))
(when fake-listener
  (define feed-xml
    (string->bytes/utf-8
     (format
      (string-append
       "<?xml version=\"1.0\"?><rss version=\"2.0\"><channel><title>Peeked</title>"
       "<author>Author</author>"
       "<item><title>New</title><pubDate>Tue, 04 Jul 2023 08:00:00 +0000</pubDate>"
       "<enclosure url=\"http://127.0.0.1:~a/audio.mp3\" type=\"audio/mpeg\"/></item>"
       "<item><title>Old</title><pubDate>Mon, 03 Jul 2023 08:00:00 +0000</pubDate>"
       "<enclosure url=\"http://127.0.0.1:~a/audio2.mp3\" type=\"audio/mpeg\"/></item>"
       "</channel></rss>")
      fake-port fake-port)))
  (define audio-bytes (make-bytes 4096 66))
  (define (fake-handle in out)
    (with-handlers ([exn:fail? void])
      (define request-line (read-line in 'return-linefeed))
      (define path (and (string? request-line)
                        (second (regexp-match #px"^GET ([^ ]+) HTTP" request-line))))
      (define body (cond [(equal? path "/feed.xml") feed-xml]
                         [(member path '("/audio.mp3" "/audio2.mp3")) audio-bytes]
                         [else #"missing"]))
      (write-bytes
       (bytes-append
        (string->bytes/utf-8
         (format "HTTP/1.1 200 OK\r\nContent-Length: ~a\r\nConnection: close\r\n\r\n"
                 (bytes-length body)))
        body)
       out)
      (close-output-port out)
      (close-input-port in)))
  (thread
   (lambda ()
     (let loop ()
       (with-handlers ([exn:fail? void])
         (define-values (in out) (tcp-accept fake-listener))
         (thread (lambda () (fake-handle in out))))
       (loop)))))

(define (fake-url path) (format "http://127.0.0.1:~a~a" fake-port path))

(dynamic-wind
 void
 (lambda ()
   (define hello (read-frame/timeout "RVT1 hello"))
   (check-equal? (frame-type hello) message:hello)
   (check-equal? (decode-value (frame-payload hello))
                 (list "rivet" protocol-version))

   (define-values (health health-error) (call-rpc "health"))
   (check-false health-error)
   (check-true (regexp-match? #rx"^podlens [0-9]" health))

   (define-values (rows rows-error) (call-rpc "settings-list"))
   (check-false rows-error)
   (check-true
    (for/or ([row (in-list rows)]) (equal? (car row) "chat-model")))

   (define-values (saved save-error)
     (call-rpc "settings-set" "chat-model" "acceptance-model"))
   (check-false save-error)
   (check-equal? saved #t)
   (define config (call-with-input-file (build-path scratch "config.json") read-json))
   (check-equal? (hash-ref config 'chat-model) "acceptance-model")

   ;; ---- preview: feed-peek + preview-audio ---------------------------------
   ;; Peeking and previewing must leave the library untouched: feed-list
   ;; stays empty after both.
   (when fake-listener
     (define-values (peek peek-error) (call-rpc "feed-peek" (fake-url "/feed.xml")))
     (check-false peek-error)
     (check-equal? (length peek) 2)
     (check-equal? (list-ref (car peek) 0) "Peeked")
     (check-equal? (list-ref (car peek) 4) "New")
     (check-true (string-contains? (list-ref (car peek) 3) "/audio.mp3"))

     (define-values (audio audio-error) (call-rpc "preview-audio" (list-ref (car peek) 3)))
     (check-false audio-error)
     (check-true (file-exists? audio))
     (check-true (string-contains? audio "preview-"))

     (define-values (feeds _) (call-rpc "feed-list"))
     (check-equal? feeds '()))
   )
 (lambda ()
   (kill-thread server-thread)
   (when fake-listener (tcp-close fake-listener))
   (delete-directory/files scratch)))
