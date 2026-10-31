#lang racket/base

;; Config validation and persistence against a temp data dir.

(require rackunit
         racket/file
         (file "../app/core/config.rkt")
         (file "../app/core/paths.rkt"))

(putenv "PODLENS_DATA_DIR" (path->string (make-temporary-file "podlens-test~a" 'directory)))

(define mgr (make-config-manager))

(test-case "defaults"
  (check-equal? (config-get mgr 'api-base) "https://api.openai.com/v1")
  (check-equal? (config-get mgr 'target-lang) "zh")
  (check-equal? (config-get mgr 'asr-model) "whisper-1"))

(test-case "set + persist"
  (config-set! mgr 'api-key "sk-test")
  (config-set! mgr 'target-lang "en")
  ;; a fresh manager sees the same values (file-backed)
  (define mgr2 (make-config-manager))
  (check-equal? (config-get mgr2 'api-key) "sk-test")
  (check-equal? (config-get mgr2 'target-lang) "en"))

(test-case "validation"
  (check-exn exn:fail:contract? (lambda () (config-set! mgr 'target-lang "fr")))
  (check-exn exn:fail:contract? (lambda () (config-set! mgr 'no-such-key "x")))
  (check-exn exn:fail:contract? (lambda () (config-set! mgr 'max-transcript-chars "not-a-number")))
  (check-exn exn:fail:contract? (lambda () (config-set! mgr 'max-transcript-chars "-1")))
  (check-equal? (config-set! mgr 'check-updates-enabled "yes") "true")
  (check-equal? (config-set! mgr 'check-updates-enabled "0") "false")
  ;; 0 must satisfy the number contract: the declared default of the
  ;; internal last-update-check used to fail its own validation and host
  ;; settings loops surfaced that as a spurious save error (#20)
  (check-equal? (config-set! mgr 'last-update-check "0") "0"))

(test-case "internal keys stay off the settings surface"
  ;; hosts echo every listed row back on save; internal bookkeeping keys
  ;; must not be listed (and a stray write to them is a no-op upstream)
  (check-false (member "last-update-check" (config-user-keys)))
  (check-not-false (member "chat-model" (config-user-keys))))

(test-case "api configured predicate"
  (config-set! mgr 'api-key "")
  (check-false (config-api-configured? mgr))
  (config-set! mgr 'api-key "sk-test")
  (check-true (config-api-configured? mgr)))
