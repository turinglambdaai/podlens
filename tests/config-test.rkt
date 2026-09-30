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
  (check-equal? (config-set! mgr 'check-updates-enabled "yes") "true")
  (check-equal? (config-set! mgr 'check-updates-enabled "0") "false"))

(test-case "api configured predicate"
  (config-set! mgr 'api-key "")
  (check-false (config-api-configured? mgr))
  (config-set! mgr 'api-key "sk-test")
  (check-true (config-api-configured? mgr)))
