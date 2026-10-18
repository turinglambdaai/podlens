#lang racket/base

(require json rackunit racket/file racket/match racket/path
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
   (check-equal? (hash-ref config 'chat-model) "acceptance-model"))
 (lambda ()
   (kill-thread server-thread)
   (delete-directory/files scratch)))
