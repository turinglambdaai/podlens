#lang racket/base

;; PodLens update wiring — the family signed-wrapper scheme (docs/UPDATE.md).
;; The update feed is a single GitHub release asset, `update-manifest.json`:
;; a self-contained wrapper (schema + base64 payload + Ed25519 signature
;; block, the rivet/distribution format). The payload is the inner manifest;
;; the signature covers the exact payload bytes every client verifies.
;;
;; The native macOS host verifies with CryptoKit (UpdateService.swift); this
;; module gives the CLI, the backend RPC and the Windows host the same check
;; by shelling out to OpenSSL 3 (the backend runs in an embedded runtime, so
;; rivet/distribution's crypto factories are deliberately not loaded here).
;; The public key is embedded raw (base64 of the 32 bytes) — the same bytes
;; the Swift host embeds — and rebuilt into a PEM for OpenSSL.
;;
;; Key management lives in docs/UPDATE.md; `scripts/update-keys.sh`
;; generates the pair. Rotation: ship a client trusting the next key
;; before signing releases exclusively with it.

(require json
         net/base64
         racket/file
         racket/format
         racket/list
         racket/port
         racket/string
         racket/system
         "core/http.rkt"
         "core/util.rkt")

(provide update-check-result
         update-configured?
         current-update-public-key-hex
         current-update-key-id
         current-update-application-id
         releases-api
         releases-page
         manifest-url
         raw-key->pem
         parse-signed-wrapper
         verify-wrapper-signature
         select-artifact)

;; Ed25519 public key, base64 of the raw 32 bytes (scripts/update-keys.sh).
;; #f keeps developer builds honest about update availability.
(define current-update-public-key-hex "yBOlLqQHWs7P5CMVwHTh+uqR3b8fA9K6K5Iwkt4csD0=")
(define current-update-key-id "podlens-2026-10")
(define current-update-application-id "site.jrtx.podlens")

(define releases-api "https://api.github.com/repos/turinglambdaai/podlens/releases/latest")
(define releases-page "https://github.com/turinglambdaai/podlens/releases/latest")

;; The moving "latest" location; redirect-following is mandatory — release
;; assets answer with a 302 to the CDN (rivet#153, followed in core/http).
(define (manifest-url)
  "https://github.com/turinglambdaai/podlens/releases/latest/download/update-manifest.json")

;; Fixed SPKI prefix bytes for an Ed25519 public key (12 bytes) followed by
;; the raw 32-byte key = full SubjectPublicKeyInfo DER.
(define ed25519-spki-prefix (hex->bytes "302a300506032b6570032100"))

(define (update-configured?)
  (and (string? current-update-public-key-hex)
       (let ([raw (base64-decode (string->bytes/latin-1 current-update-public-key-hex))])
         (and raw (= 32 (bytes-length raw))))))

;; raw 32-byte key → SubjectPublicKeyInfo PEM
(define (raw-key->pem raw-32)
  (define der (bytes-append ed25519-spki-prefix raw-32))
  ;; net/base64 on this Racket build appends its second argument verbatim
  ;; and the default value is garbage (a temp-file-shaped string) — pass ""
  ;; explicitly and trim the trailing newline
  (define b64 (string-trim (bytes->string/latin-1 (base64-encode der ""))))
  (define wrapped (regexp-match* #px".{1,64}" b64))
  (string-append
   "-----BEGIN PUBLIC KEY-----\n"
   (string-join wrapped "\n")
   "\n-----END PUBLIC KEY-----\n"))

(define (find-openssl)
  (define candidates
    (list (getenv "OPENSSL_BIN")
          "/opt/homebrew/opt/openssl@3/bin/openssl"
          "/usr/local/opt/openssl@3/bin/openssl"))
  (or (for/or ([c (in-list candidates)] #:when (and c (file-exists? c))) c)
      (find-executable-path "openssl")))

(define (openssl3? path)
  (and path
       (string-contains?
        (with-output-to-string
          (lambda ()
            (with-handlers ([exn:fail? void])
              (system* path "version"))))
        "OpenSSL 3")))

;; ---- signed wrapper (pure parsing, shared with the tests) -----------------

;; jsexpr → (values payload key-id signature); raises when the envelope is
;; not the family signed-wrapper format (schema 1, ed25519, base64 parts).
(define (parse-signed-wrapper wrapper)
  (unless (hash? wrapper)
    (error 'parse-signed-wrapper "manifest is not a JSON object"))
  (define schema (hash-ref wrapper 'schema
                           (lambda () (error 'parse-signed-wrapper "schema field missing"))))
  (unless (equal? schema 1)
    (error 'parse-signed-wrapper "unsupported wrapper schema: ~a" schema))
  (define sig-block (hash-ref wrapper 'signature
                              (lambda () (error 'parse-signed-wrapper "signature block missing"))))
  (unless (hash? sig-block)
    (error 'parse-signed-wrapper "signature block is not an object"))
  (define algorithm (hash-ref sig-block 'algorithm #f))
  (unless (equal? algorithm "ed25519")
    (error 'parse-signed-wrapper "unsupported signature algorithm: ~a" algorithm))
  (define key-id (hash-ref sig-block 'key_id #f))
  (define payload
    (or (and (hash-has-key? wrapper 'payload)
             (let ([v (hash-ref wrapper 'payload)])
               (and (string? v) (base64-decode (string->bytes/latin-1 v)))))
        (error 'parse-signed-wrapper "payload is not valid base64")))
  (define signature
    (or (and (hash-has-key? sig-block 'value)
             (let ([v (hash-ref sig-block 'value)])
               (and (string? v) (base64-decode (string->bytes/latin-1 v)))))
        (error 'parse-signed-wrapper "signature value is not valid base64")))
  (unless (= 64 (bytes-length signature))
    (error 'parse-signed-wrapper "signature is not a 64-byte Ed25519 signature"))
  (values payload (and (string? key-id) key-id) signature))

;; Ed25519 verify via OpenSSL 3: signature over the exact payload bytes,
;; against the embedded raw public key. Raises when verification fails or
;; no OpenSSL 3 is available.
(define (verify-wrapper-signature payload signature key-raw)
  (unless (and (bytes? key-raw) (= 32 (bytes-length key-raw)))
    (error 'verify-wrapper-signature "embedded update key is malformed"))
  (define openssl (find-openssl))
  (unless (openssl3? openssl)
    (error 'verify-wrapper-signature "OpenSSL 3 not found for signature verification"))
  (define tmp (make-temporary-file "podlens-update~a"))
  (define payload-file (path-replace-extension tmp #".payload"))
  (define sig-file (path-replace-extension tmp #".sig"))
  (define pem-file (path-replace-extension tmp #".pem"))
  (dynamic-wind
    (lambda () (void))
    (lambda ()
      (display-to-file payload payload-file #:exists 'replace)
      (display-to-file signature sig-file #:exists 'replace)
      (display-to-file (raw-key->pem key-raw) pem-file #:exists 'replace)
      (define out
        (with-output-to-string
          (lambda ()
            (system* openssl "pkeyutl" "-verify" "-pubin"
                     "-inkey" pem-file "-rawin"
                     "-in" payload-file "-sigfile" sig-file))))
      (unless (string-contains? out "Signature Verified Successfully")
        (error 'verify-wrapper-signature "manifest signature invalid")))
    (lambda ()
      (with-handlers ([exn:fail? void])
        (delete-file payload-file)
        (delete-file sig-file)
        (delete-file pem-file)))))

;; inner manifest jsexpr × platform symbol × architecture symbol → artifact
;; jsexpr or #f. The feed carries one artifact per platform × architecture.
(define (select-artifact manifest platform architecture)
  (define platform-s (symbol->string platform))
  (define arch-s (symbol->string architecture))
  (for/or ([artifact (in-list (hash-ref manifest 'artifacts '()))]
           #:when (hash? artifact))
    (and (equal? (hash-ref artifact 'platform #f) platform-s)
         (equal? (hash-ref artifact 'architecture #f) arch-s)
         artifact)))

;; rivet release tooling emits these exact platform/architecture names into
;; update manifests (same mapping the rivet MSI tooling uses).
(define (host-platform)
  (case (system-type 'os)
    [(macosx) 'macos]
    [(windows) 'windows]
    [else 'linux]))

(define (host-architecture)
  (case (system-type 'arch)
    [(aarch64 arm64) 'arm64]
    [else 'x64]))

;; → structured result the callers localize:
;;   '(up-to-date) | (list 'available version)
;;   (list 'unavailable reason) | (list 'failed reason)
;;
;; The macOS host runs the same contract natively; this module gives the
;; CLI, the backend RPC and the Windows host the same check by shelling
;; out to OpenSSL 3.
(define (update-check-result current-version)
  (cond
    [(not (update-configured?))
     (list 'unavailable "developer build (no update key configured)")]
    [else
     (with-handlers
         ([exn:fail? (lambda (e) (list 'failed (exn-message e)))])
       (define-values (code _h body)
         (http-get-bytes (manifest-url) (list "Accept: application/vnd.github+json")))
       (unless (= code 200)
         (error 'update-check "~a returned ~a" (manifest-url) code))
       (define wrapper (bytes->jsexpr body))
       (define-values (payload key-id signature) (parse-signed-wrapper wrapper))
       (unless (equal? key-id current-update-key-id)
         (error 'update-check "manifest signed by unexpected key ~a" key-id))
       (verify-wrapper-signature
        payload signature
        (base64-decode (string->bytes/latin-1 current-update-public-key-hex)))
       (define manifest (bytes->jsexpr payload))
       (unless (equal? (hash-ref manifest 'application_id #f)
                       current-update-application-id)
         (error 'update-check "manifest is for a different application"))
       (unless (select-artifact manifest (host-platform) (host-architecture))
         (error 'update-check "feed carries no artifact for this platform"))
       (define latest (hash-ref manifest 'version "0"))
       (if (version-newer? current-version latest)
           (list 'available latest)
           (list 'up-to-date)))]))

(define (version-newer? current latest)
  (define (segs v)
    (map (lambda (p) (or (string->number p) 0)) (string-split v ".")))
  (define a (segs current))
  (define b (segs latest))
  (define n (max (length a) (length b)))
  (define (pad xs) (append xs (make-list (- n (length xs)) 0)))
  (for/or ([x (in-list (pad a))] [y (in-list (pad b))]) (> y x)))
