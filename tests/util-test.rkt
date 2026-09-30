#lang racket/base

;; Unit tests for the shared helpers. Run: raco test tests/

(require rackunit
         (file "../app/core/util.rkt"))

(test-case "bytes->hex left-pads"
  (check-equal? (bytes->hex #"\x01\x0f\xfe") "010ffe"))

(test-case "hex->bytes round-trip"
  (check-equal? (hex->bytes (bytes->hex #"podlens")) #"podlens")
  (check-equal? (hex->bytes "xyz") #f))

(test-case "stable-id is deterministic"
  (check-equal? (stable-id "abc") (stable-id "abc"))
  (check-not-equal? (stable-id "abc") (stable-id "abd"))
  (check-equal? (string-length (stable-id "x")) 40))
