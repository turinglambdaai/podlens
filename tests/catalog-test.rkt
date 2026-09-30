#lang racket/base

;; The shipped catalog must stay honest: unique ids, https URLs, known
;; categories, non-empty bilingual copy. Live reachability is verified
;; separately (scripts/verify-catalog.rkt) at release time.

(require rackunit
         racket/list
         racket/string
         (file "../app/core/catalog.rkt"))

(test-case "catalog shape"
  (define es (catalog-entries))
  (check-true (>= (length es) 10) "catalog should carry a meaningful selection")
  (check-equal? (length es)
                (length (remove-duplicates (map catalog-entry-id es)))
                "ids must be unique")
  (for ([e (in-list es)])
    (check-true (string-prefix? (catalog-entry-url e) "https://")
                (format "~a: url must be https" (catalog-entry-id e)))
    (check-true (and (member (catalog-entry-category e)
                             (list "tech" "security" "science" "design" "business" "news"))
                     #t)
                (format "~a: unknown category" (catalog-entry-id e)))
    (check-true (non-empty-string? (catalog-entry-description e "zh"))
                (format "~a: missing zh description" (catalog-entry-id e)))
    (check-true (non-empty-string? (catalog-entry-description e "en"))
                (format "~a: missing en description" (catalog-entry-id e)))
    (check-true (non-empty-string? (catalog-entry-homepage e))
                (format "~a: missing homepage" (catalog-entry-id e)))))
