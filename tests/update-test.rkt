#lang racket/base

;; Update-channel regression tests — all offline. The trust chain uses an
;; in-process throwaway Ed25519 key through the same crypto libcrypto
;; factory rivet/distribution pins (no OpenSSL 3 subprocess, no PATH
;; dependence); download coverage runs against a loopback HTTP server
;; standing in for the release CDN. The live check path is pointed at a
;; closed loopback port so a call-shape regression (an illegal keyword, as
;; shipped in payback v1.4.1–v1.6.2) fails the suite instead of every
;; user's check-for-update.

(require crypto
         json
         rackunit
         net/base64
         rivet/distribution
         racket/file
         racket/list
         racket/port
         racket/string
         racket/tcp
         (file "../app/update.rkt"))

(define (file-sha256-hex path) (sha256-file/hex path))

;; ---- configuration and identity -------------------------------------------

(test-case "update-configured? accepts the embedded key"
  (check-true (update-configured?)))

(test-case "identity symbols match rivet release manifests"
  (check-equal? (platform-symbol)
                (case (system-type 'os)
                  [(macosx) 'macos] [(windows) 'windows] [else 'linux]))
  (check-not-false (memq (architecture-symbol) '(arm64 x64))))

(test-case "embedded public key decodes as SPKI DER"
  ;; update-public-key-b64 is the raw 32 bytes; the SPKI prefix turns it
  ;; into the DER blob the crypto library parses (same construction the
  ;; module uses for its embedded key).
  (define raw (base64-string->bytes update-public-key-b64))
  (check-equal? (bytes-length raw) 32)
  ;; 12-byte fixed Ed25519 SPKI prefix + the raw key = parseable DER (the
  ;; same construction the module applies to its embedded key).
  (define spki-prefix
    (bytes-append (bytes #x30 #x2a #x30 #x05 #x06 #x03 #x2b #x65 #x70)
                  (bytes #x03 #x21 #x00)))
  (check-true
   (pk-key?
    (datum->pk-key (bytes-append spki-prefix raw)
                   'SubjectPublicKeyInfo))))

;; ---- signed-wrapper trust chain (in-process Ed25519) -----------------------

(test-case "signed manifest verifies and selects updates"
  ;; throwaway keypair in the same DER formats the release pipeline uses
  (define priv (generate-private-key 'eddsa '((curve ed25519))))
  (define priv-der (pk-key->datum priv 'OneAsymmetricKey))
  ;; derive the public half through the library: the rkt-private datum
  ;; element order differs between generated and DER-imported keys
  (define pub (datum->pk-key (pk-key->datum priv 'rkt-public) 'rkt-public))
  (define pub-der (pk-key->datum pub 'SubjectPublicKeyInfo))

  (define tmp (make-temporary-file "podlens-updater-~a" 'directory))
  (define manifest-path (build-path tmp "update-manifest.json"))
  (define artifact
    (update-artifact 'macos 'arm64
                     "https://example.invalid/podlens-1.4.0-macos-arm64.zip"
                     "0000000000000000000000000000000000000000000000000000000000000000"
                     123456789 'zip '()))
  (define manifest
    (update-manifest app-identifier "1.4.0" 9 'stable
                     "2026-10-09T00:00:00Z" "0.0.0" #f #t 100
                     (list artifact)))
  (call-with-output-file manifest-path
    (lambda (o) (write-signed-manifest manifest
                                       (read-ed25519-private-key
                                        (let ([p (build-path tmp "priv.der")])
                                          (call-with-output-file p
                                            (lambda (o) (write-bytes priv-der o))
                                            #:exists 'truncate/replace)
                                          p))
                                       update-key-id o)
      (newline o)))

  ;; tamper check first: a modified payload must fail verification
  (define signed-bytes (file->bytes manifest-path))
  (define tampered
    (bytes-append (subbytes signed-bytes 0 40)
                  #"9"
                  (subbytes signed-bytes 41)))
  (check-exn exn:fail?
             (lambda ()
               (verify-signed-manifest (open-input-bytes tampered)
                                       (datum->pk-key pub-der 'SubjectPublicKeyInfo)
                                       #:key-id update-key-id)))

  (define verified
    (verify-signed-manifest (open-input-bytes signed-bytes)
                            (datum->pk-key pub-der 'SubjectPublicKeyInfo)
                            #:key-id update-key-id))
  (check-equal? (update-manifest-version verified) "1.4.0")

  ;; selection policy: newer version for this app/channel/platform wins
  (define config
    (updater-config app-identifier app-version 'stable
                    (platform-symbol) (architecture-symbol)
                    (datum->pk-key pub-der 'SubjectPublicKeyInfo)
                    update-key-id 42 (* 800 1024 1024)))
  (define (selectable manifest)
    (update-candidate? (select-update config manifest)))
  (check-true (selectable verified))
  (check-false (selectable (struct-copy update-manifest verified
                                        [version app-version]))
               "same version: nothing to do")
  (check-false (selectable (struct-copy update-manifest verified
                                        [channel 'beta]))
               "other channel is invisible")
  (check-false (selectable (struct-copy update-manifest verified
                                        [rollout 42]))
               "bucket 42 is excluded when rollout is 42")
  (check-true (selectable (struct-copy update-manifest verified
                                       [rollout 43])))
  (check-exn exn:fail?
             (lambda ()
               (select-update
                config
                (struct-copy update-manifest verified
                             [application-id "com.other.app"])))
             "other application identity must fail loudly")
  (with-handlers ([exn:fail? void])
    (delete-directory/files tmp)))

;; ---- live check path fails on network grounds, never on call shape --------

(test-case "perform-check! against a closed port reports a network error"
  (define result
    (parameterize ([current-update-base-url "https://127.0.0.1:1"])
      (perform-check!)))
  (check-equal? (hash-ref result 'status) "error")
  (check-false
   (string-contains? (hash-ref result 'message "")
                     "given keyword")
   "check must not fail on its own call shape (illegal keyword regression)")
  (check-equal? (hash-ref (update-state-snapshot) 'phase) "error")
  (reset-update-state!))

;; ---- state machine ----------------------------------------------------------

(test-case "update state starts idle and resets cleanly"
  (reset-update-state!)
  (define s (update-state-snapshot))
  (check-equal? (hash-ref s 'phase) "idle")
  (check-equal? (hash-ref s 'percent) 0)
  (check-false (hash-ref s 'message))
  (check-false (hash-ref s 'downloadedPath)))

(test-case "set-update-error! surfaces through the snapshot"
  (reset-update-state!)
  (set-update-error! "boom")
  (check-equal? (hash-ref (update-state-snapshot) 'phase) "error")
  (check-equal? (hash-ref (update-state-snapshot) 'message) "boom")
  (reset-update-state!))

(test-case "start-download! refuses to run without a candidate"
  (reset-update-state!)
  (set-candidate! #f)
  (check-exn exn:fail?
             (lambda ()
               (start-download! (make-temporary-file "podlens-dl~a" 'directory)))))

(test-case "destination-path lands in <data-dir>/updates/ with a self-describing name"
  (define tmp (make-temporary-file "podlens-dl~a" 'directory))
  (define candidate
    (update-candidate
     (update-manifest app-identifier "9.9.9" 99 'stable
                      "2026-10-13T00:00:00Z" "0.0.0" #f #t 100
                      (list (update-artifact 'macos 'arm64
                                             "https://example.invalid/x.zip"
                                             "00" 1 'zip '())))
     (update-artifact 'macos 'arm64
                      "https://example.invalid/x.zip"
                      "00" 1 'zip '())))
  (check-equal?
   (path->string (destination-path tmp candidate))
   (path->string
    (build-path tmp "updates" (string-append app-display-name "-9.9.9"
                                             (installer-extension))))))

;; ---- download against a loopback CDN ---------------------------------------

(define artifact-payload (make-bytes 3000 7))

(define-values (artifact-port artifact-listener artifact-server-thread)
  (let loop ([port 18900])
    (with-handlers ([exn:fail:network? (lambda (_) (loop (add1 port)))])
      (define listener (tcp-listen port 4 #f "127.0.0.1"))
      (define server
        (thread
         (lambda ()
           (with-handlers ([exn:fail? void])
             (let accept-loop ()
               (define-values (in out) (tcp-accept listener))
               (thread
                (lambda ()
                  (with-handlers ([exn:fail? void])
                    (read-line in 'any)
                    (let drain ()
                      (define line (read-line in 'any))
                      (unless (or (eof-object? line) (equal? line "")) (drain)))
                    (fprintf out
                             "HTTP/1.1 200 OK\r\nContent-Length: ~a\r\n"
                             (bytes-length artifact-payload))
                    (fprintf out "Connection: close\r\nContent-Type: application/zip\r\n\r\n")
                    (write-bytes artifact-payload out)
                    (flush-output out)
                    (close-output-port out)
                    (close-input-port in))))
               (accept-loop))))))
      (values port listener server))))

(define artifact-work-dir (make-temporary-file "podlens-dl~a" 'directory))
(define artifact-url
  (format "http://127.0.0.1:~a/podlens-9.9.9-windows-x64.zip" artifact-port))
(define artifact-sha
  (let ([probe (build-path artifact-work-dir "probe.bin")])
    (display-to-file artifact-payload probe #:exists 'replace #:mode 'binary)
    (file-sha256-hex probe)))

(define (wait-for-terminal-state!)
  ;; phase and message are written as two separate state updates, so an
  ;; error landing means the message may still be one write behind — wait
  ;; for the pair, not just the phase.
  (let wait ([n 400])
    (define s (update-state-snapshot))
    (define phase (hash-ref s 'phase))
    (define settled?
      (or (equal? phase "downloaded")
          (and (equal? phase "error") (hash-ref s 'message #f))))
    (unless settled?
      (when (> n 0)
        (sleep 0.05)
        (wait (sub1 n))))))

(when (and artifact-port (string? artifact-sha))
  (test-case "artifact download verifies and lands at the destination"
    (reset-update-state!)
    (set-candidate!
     (update-candidate
      (update-manifest app-identifier "9.9.9" 99 'stable
                       "2026-10-13T00:00:00Z" "0.0.0" #f #t 100
                       (list (update-artifact 'macos 'arm64 artifact-url
                                              artifact-sha
                                              (bytes-length artifact-payload)
                                              'zip '())))
      (update-artifact 'macos 'arm64 artifact-url
                       artifact-sha
                       (bytes-length artifact-payload)
                       'zip '())))
    (start-download! artifact-work-dir)
    (wait-for-terminal-state!)
    (define s (update-state-snapshot))
    (check-equal? (hash-ref s 'phase) "downloaded")
    (check-equal? (hash-ref s 'percent) 100)
    (define path (hash-ref s 'downloadedPath))
    (check-true (and path (file-exists? path)))
    (check-equal? (file->bytes path) artifact-payload)
    (check-equal? (file-sha256-hex path) artifact-sha))

  (test-case "checksum mismatch fails the download and leaves no artifact"
    (define destination
      (build-path artifact-work-dir "updates" "podlens-9.9.9-windows-x64.zip"))
    (when (file-exists? destination) (delete-file destination))
    (reset-update-state!)
    (set-candidate!
     (update-candidate
      (update-manifest app-identifier "9.9.9" 99 'stable
                       "2026-10-13T00:00:00Z" "0.0.0" #f #t 100
                       (list (update-artifact 'macos 'arm64 artifact-url
                                              (string-append artifact-sha "0000")
                                              (bytes-length artifact-payload)
                                              'zip '())))
      (update-artifact 'macos 'arm64 artifact-url
                       (string-append artifact-sha "0000")
                       (bytes-length artifact-payload)
                       'zip '())))
    (start-download! artifact-work-dir)
    (wait-for-terminal-state!)
    (define s (update-state-snapshot))
    (check-equal? (hash-ref s 'phase) "error")
    (check-true
     (string-contains? (string-downcase (hash-ref s 'message "")) "sha")
     "the digest failure must surface by name (rivet: SHA-256 mismatch)")
    (check-false (file-exists? destination))
    (check-false
     (file-exists?
      (path-add-extension destination #".partial"))))

  (test-case "size mismatch fails the download"
    (reset-update-state!)
    (set-candidate!
     (update-candidate
      (update-manifest app-identifier "9.9.9" 99 'stable
                       "2026-10-13T00:00:00Z" "0.0.0" #f #t 100
                       (list (update-artifact 'macos 'arm64 artifact-url
                                              artifact-sha
                                              (add1 (bytes-length artifact-payload))
                                              'zip '())))
      (update-artifact 'macos 'arm64 artifact-url
                       artifact-sha
                       (add1 (bytes-length artifact-payload))
                       'zip '())))
    (start-download! artifact-work-dir)
    (wait-for-terminal-state!)
    (check-equal? (hash-ref (update-state-snapshot) 'phase) "error")
    (check-true (string-contains? (hash-ref (update-state-snapshot) 'message "") "size"))))

(kill-thread artifact-server-thread)
(tcp-close artifact-listener)
(reset-update-state!)
(set-candidate! #f)
