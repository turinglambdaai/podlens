#lang racket/base

;; Unit tests for the shared helpers. Run: raco test tests/

(require rackunit
         (file "../app/core/util.rkt"))

(test-case "sha256 FIPS vectors"
  (check-equal? (bytes->hex (sha256 #"")) "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
  (check-equal? (bytes->hex (sha256 #"abc")) "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
  (check-equal?
   (bytes->hex (sha256 #"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))
   "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")
  (check-equal?
   (bytes->hex (sha256 (make-bytes 1000000 97)))
   "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"))

(test-case "bytes->hex left-pads"
  (check-equal? (bytes->hex #"\x01\x0f\xfe") "010ffe"))

(test-case "hex->bytes round-trip"
  (check-equal? (hex->bytes (bytes->hex #"podlens")) #"podlens")
  (check-equal? (hex->bytes "xyz") #f))

(test-case "stable-id is deterministic"
  (check-equal? (stable-id "abc") (stable-id "abc"))
  (check-not-equal? (stable-id "abc") (stable-id "abd"))
  (check-equal? (string-length (stable-id "x")) 40))
