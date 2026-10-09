#lang racket/base

;; Update-channel regression tests. No network; the signature round-trip
;; uses a throwaway OpenSSL 3 key and is skipped where no OpenSSL 3 exists
;; (e.g. macOS CI runners without homebrew openssl@3 — the ubuntu leg
;; covers it). The 1.3.0 cycle found that raw-key->pem emitted a stray "#f"
;; into the base64 body (this Racket build's base64-encode appends its
;; second argument verbatim), which made every CLI/backend signature
;; verification fail.

(require json
         rackunit
         net/base64
         racket/file
         racket/list
         racket/port
         racket/string
         racket/system
         (file "../app/update.rkt"))

(define key-raw (base64-decode (string->bytes/latin-1
                                current-update-public-key-hex)))

(test-case "raw-key->pem emits clean SPKI PEM"
  (define pem (raw-key->pem key-raw))
  (define lines (string-split pem "\n"))
  (check-equal? (first lines) "-----BEGIN PUBLIC KEY-----")
  (check-equal? (last lines) "-----END PUBLIC KEY-----")
  ;; the DER body: one or more pure-base64 lines, no stray markers
  (define body (take (drop lines 1) (- (length lines) 2)))
  (check-true (and (pair? body) #t))
  (for ([line (in-list body)])
    (check-regexp-match #px"^[A-Za-z0-9+/]+={0,2}$" line))
  ;; the body must decode back to the 12-byte SPKI prefix + 32-byte key
  (define der (base64-decode (string->bytes/latin-1 (string-append* body))))
  (check-equal? (bytes-length der) 44))

(test-case "update-configured? accepts the embedded key"
  (check-true (update-configured?)))

;; ---- signed wrapper (v1.4.0 family format) --------------------------------

(define sample-manifest
  (hasheq 'schema 1
          'application_id current-update-application-id
          'version "1.4.0"
          'build 9
          'channel "stable"
          'published_at "2026-10-09T00:00:00Z"
          'minimum_version "0.0.0"
          'previous_version #f
          'rollback_allowed #t
          'rollout 100
          'artifacts
          (list (hasheq 'platform "macos" 'architecture "arm64"
                        'url "https://example.invalid/podlens-1.4.0-macos-arm64.zip"
                        'sha256 "aa" 'size 1 'installer "zip" 'arguments '())
                (hasheq 'platform "macos" 'architecture "x64"
                        'url "https://example.invalid/podlens-1.4.0-macos-x64.zip"
                        'sha256 "bb" 'size 2 'installer "zip" 'arguments '())
                (hasheq 'platform "windows" 'architecture "x64"
                        'url "https://example.invalid/podlens-1.4.0-windows-x64.zip"
                        'sha256 "cc" 'size 3 'installer "zip" 'arguments '()))))

(define (wrapper->jsexpr wrapper)
  (read-json (open-input-string wrapper)))

(test-case "parse-signed-wrapper accepts a well-formed wrapper"
  (define payload (jsexpr->bytes sample-manifest))
  (define wrapper
    (wrapper->jsexpr
     (format "{\"schema\":1,\"payload\":\"~a\",\"signature\":{\"algorithm\":\"ed25519\",\"key_id\":\"~a\",\"value\":\"~a\"}}"
             (bytes->string/latin-1 (base64-encode payload ""))
             current-update-key-id
             (bytes->string/latin-1 (base64-encode (make-bytes 64 1) "")))))
  (define-values (got-payload key-id signature)
    (parse-signed-wrapper wrapper))
  (check-equal? got-payload payload)
  (check-equal? key-id current-update-key-id)
  (check-equal? (bytes-length signature) 64))

(test-case "parse-signed-wrapper rejects malformed wrappers"
  (define (reject? thunk)
    (with-handlers ([exn:fail? (lambda (_) #t)]) (thunk) #f))
  (check-true (reject? (lambda () (parse-signed-wrapper "not-json"))))
  (check-true
   (reject?
    (lambda ()
      (parse-signed-wrapper
       (hasheq 'schema 2 'payload
               (bytes->string/latin-1 (base64-encode #"x" ""))
               'signature (hasheq 'algorithm "ed25519"
                                  'key_id current-update-key-id
                                  'value (bytes->string/latin-1
                                          (base64-encode (make-bytes 64 1) ""))))))))
  (check-true
   (reject?
    (lambda ()
      (parse-signed-wrapper
       (hasheq 'schema 1 'payload
               (bytes->string/latin-1 (base64-encode #"x" ""))
               'signature (hasheq 'algorithm "rsa"
                                  'key_id current-update-key-id
                                  'value (bytes->string/latin-1
                                          (base64-encode (make-bytes 64 1) ""))))))))
  ;; signature shorter than 64 bytes
  (check-true
   (reject?
    (lambda ()
      (parse-signed-wrapper
       (hasheq 'schema 1 'payload
               (bytes->string/latin-1 (base64-encode #"x" ""))
               'signature (hasheq 'algorithm "ed25519"
                                  'key_id current-update-key-id
                                  'value (bytes->string/latin-1
                                          (base64-encode (make-bytes 32 1) "")))))))))

(test-case "select-artifact matches platform × architecture"
  (check-false (select-artifact sample-manifest 'macos 'x86))
  (check-equal? (hash-ref (select-artifact sample-manifest 'macos 'arm64) 'sha256) "aa")
  (check-equal? (hash-ref (select-artifact sample-manifest 'macos 'x64) 'sha256) "bb")
  (check-equal? (hash-ref (select-artifact sample-manifest 'windows 'x64) 'sha256) "cc"))

;; ---- signature round-trip (only where a real OpenSSL 3 exists) -------------

(define (openssl3-available?)
  (define candidates
    (list (getenv "OPENSSL_BIN")
          "/opt/homebrew/opt/openssl@3/bin/openssl"
          "/usr/local/opt/openssl@3/bin/openssl"
          (find-executable-path "openssl")))
  (for/or ([c (in-list candidates)] #:when (and c (file-exists? c) #t))
    (and c
         (string-contains?
          (with-output-to-string
            (lambda () (with-handlers ([exn:fail? void]) (system* c "version"))))
          "OpenSSL 3")
         c)))

(define openssl (openssl3-available?))

(when openssl
  (test-case "wrapper signature round-trips through OpenSSL 3 Ed25519"
    (define work-dir (make-temporary-file "podlens-key~a" 'directory))
    (dynamic-wind
      (lambda () (void))
      (lambda ()
        (define key-pem (build-path work-dir "key.pem"))
        (system* openssl "genpkey" "-algorithm" "ed25519"
                 "-out" (path->string key-pem))
        ;; raw key = last 32 bytes of the SPKI DER
        (define pub-der
          (with-output-to-bytes
            (lambda ()
              (system* openssl "pkey" "-in" (path->string key-pem)
                       "-pubout" "-outform" "DER"))))
        (define raw-key
          (subbytes pub-der (- (bytes-length pub-der) 32)))
        (check-equal? (bytes-length raw-key) 32)
        (define payload (jsexpr->bytes sample-manifest))
        (define payload-file (build-path work-dir "payload.bin"))
        (display-to-file payload payload-file #:exists 'replace)
        (define sig-file (build-path work-dir "payload.sig"))
        (system* openssl "pkeyutl" "-sign" "-inkey" (path->string key-pem)
                 "-rawin" "-in" (path->string payload-file)
                 "-out" (path->string sig-file))
        (define signature (file->bytes sig-file))
        (check-equal? (bytes-length signature) 64)
        ;; the right key verifies
        (check-not-exn
         (lambda () (verify-wrapper-signature payload signature raw-key)))
        ;; a tampered payload must fail verification
        (check-exn exn:fail?
                   (lambda ()
                     (verify-wrapper-signature
                      (bytes-append payload #" ") signature raw-key))))
      (lambda ()
        (with-handlers ([exn:fail? void])
          (delete-directory/files work-dir))))))
