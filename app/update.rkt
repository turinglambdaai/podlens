#lang racket/base

;; PodLens update wiring — the Taskly scheme: the GitHub Releases API of
;; this repo carries two assets, `update-manifest.json` and `manifest.sig`
;; (raw 64-byte Ed25519 signature over the exact manifest bytes). The
;; manifest maps platform → {url, sha256, size}.
;;
;; The native hosts verify with platform crypto (CryptoKit on macOS); this
;; module gives the CLI the same check by shelling out to OpenSSL 3. The
;; public key is embedded raw (last 32 bytes of the SPKI DER, base64) —
;; the same bytes the Swift host embeds — and rebuilt into a PEM here.
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
         releases-api
         releases-page
         raw-key->pem)

;; Ed25519 public key, base64 of the raw 32 bytes (scripts/update-keys.sh).
;; #f keeps developer builds honest about update availability.
(define current-update-public-key-hex "eWk+MVBTRUkcf3O4HSKek5yZ+cEv1oyx4QEErjC4opA=")
(define current-update-key-id "release-2026")

(define releases-api "https://api.github.com/repos/turinglambdaai/podlens/releases/latest")
(define releases-page "https://github.com/turinglambdaai/podlens/releases/latest")

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
  ;; net/base64 appends a CRLF by default and treats any second argument as
  ;; literal suffix text on this Racket build — so take the default and trim
  (define b64 (string-trim (bytes->string/latin-1 (base64-encode der))))
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

;; → structured result the callers localize:
;;   '(up-to-date) | (list 'available version)
;;   (list 'unavailable reason) | (list 'failed reason)
;;
;; The macOS host runs the same contract natively; this module gives the
;; CLI and the backend RPC the same check by shelling out to OpenSSL 3.
(define (update-check-result current-version)
  (cond
    [(not (update-configured?))
     (list 'unavailable "developer build (no update key configured)")]
    [else
     (with-handlers
         ([exn:fail? (lambda (e) (list 'failed (exn-message e)))])
       (define (get-json url)
         (define-values (code _h body)
           (http-get-bytes url (list "Accept: application/vnd.github+json")))
         (unless (= code 200)
           (error 'update-check "~a returned ~a" url code))
         (bytes->jsexpr body))
       (define release (get-json releases-api))
       (define assets (hash-ref release 'assets '()))
       (define (asset-url name)
         (for/or ([a (in-list assets)])
           (and (equal? (hash-ref a 'name "") name)
                (hash-ref a 'browser_download_url #f))))
       (define manifest-url (asset-url "update-manifest.json"))
       (define sig-url (asset-url "manifest.sig"))
       (unless (and manifest-url sig-url)
         (error 'update-check "release ~a carries no update manifest"
                (hash-ref release 'tag_name "?")))
       (define manifest-bytes (let-values ([(_c _h b) (http-get-bytes manifest-url)]) b))
       (define sig-bytes (let-values ([(_c _h b) (http-get-bytes sig-url)]) b))
       ;; Ed25519 verify via OpenSSL 3
       (define openssl (find-openssl))
       (unless (openssl3? openssl)
         (error 'update-check "OpenSSL 3 not found for signature verification"))
       (define key-raw
         (base64-decode (string->bytes/latin-1 current-update-public-key-hex)))
       (unless (and (bytes? key-raw) (= 32 (bytes-length key-raw)))
         (error 'update-check "embedded update key is malformed"))
       (define tmp (make-temporary-file "podlens-update~a"))
       (define manifest-file (path-replace-extension tmp #".json"))
       (define sig-file (path-replace-extension tmp #".sig"))
       (define pem-file (path-replace-extension tmp #".pem"))
       (dynamic-wind
         (lambda () (void))
         (lambda ()
           (display-to-file manifest-bytes manifest-file #:exists 'replace)
           (display-to-file sig-bytes sig-file #:exists 'replace)
           (display-to-file (raw-key->pem key-raw) pem-file #:exists 'replace)
           (define out
             (with-output-to-string
               (lambda ()
                 (system* openssl "pkeyutl" "-verify" "-pubin"
                          "-inkey" pem-file "-rawin"
                          "-in" manifest-file "-sigfile" sig-file))))
           (unless (string-contains? out "Signature Verified Successfully")
             (error 'update-check "manifest signature invalid"))
           (define manifest (with-input-from-file manifest-file read-json))
           (define latest (hash-ref manifest 'version "0"))
           (if (version-newer? current-version latest)
               (list 'available latest)
               (list 'up-to-date)))
         (lambda ()
           (with-handlers ([exn:fail? void])
             (delete-file manifest-file)
             (delete-file sig-file)
             (delete-file pem-file)))))]))

(define (version-newer? current latest)
  (define (segs v)
    (map (lambda (p) (or (string->number p) 0)) (string-split v ".")))
  (define a (segs current))
  (define b (segs latest))
  (define n (max (length a) (length b)))
  (define (pad xs) (append xs (make-list (- n (length xs)) 0)))
  (for/or ([x (in-list (pad a))] [y (in-list (pad b))]) (> y x)))
