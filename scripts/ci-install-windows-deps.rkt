#lang racket/base
;; GitHub-hosted Windows Racket 9.3 may crash while reading Windows ROOT
;; certificates (X509*->C). Use the runner's independent Mozilla CA roots
;; in the SAME Racket process that downloads packages. Never disable TLS
;; certificate or hostname verification.
(require openssl pkg racket/file)
(define bundle (getenv "PODLENS_CI_CA_BUNDLE"))
(unless (and bundle (file-exists? bundle)
             (regexp-match? #rx#"-----BEGIN CERTIFICATE-----"
                            (file->bytes bundle)))
  (error 'ci-install-windows-deps "trusted Mozilla PEM CA bundle is missing or invalid"))
(ssl-default-verify-sources (list bundle))
(define rivet-pin (getenv "RIVET_PIN"))
(unless (and rivet-pin (not (equal? rivet-pin "")))
  (error 'ci-install-windows-deps "RIVET_PIN is missing"))
(define acceptance?
  (for/or ([arg (in-vector (current-command-line-arguments))])
    (equal? arg "--acceptance")))
(when acceptance?
  (define keel-pin (getenv "KEEL_PIN"))
  (unless (and keel-pin (not (equal? keel-pin "")))
    (error 'ci-install-windows-deps "KEEL_PIN is missing"))
  (pkg-install-command #:auto #t #:no-docs #t
                       (format "https://github.com/turlamai/keel.git#~a" keel-pin)))
(pkg-install-command #:auto #t #:no-docs #t
                     (format "https://github.com/turlamai/rivet.git#~a" rivet-pin))
(when acceptance?
  (pkg-install-command #:auto #t #:no-docs #t #:skip-installed #t "crypto"))
