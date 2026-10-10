#lang racket/base

;; Product DTOs for the update flow — the only structured rows in the
;; PodLens wire contract (everything else is positional strings). Rivet
;; keeps these as named records in Racket/native code while RVT1 continues
;; to encode them as field-ordered lists for wire compatibility.

(require rivet/backend)

(provide (all-defined-out))

;; Result of update-check. status: "available" | "up-to-date" | "error";
;; the descriptive fields are only filled for "available".
(define-record UpdateCheck
  ([status : String]
   [error : (Optional String)]
   [current-version : String]
   [available-version : (Optional String)]
   [build : (Optional Int64)]
   [published-at : (Optional String)]
   [installer : (Optional String)]
   [size-bytes : (Optional Int64)]))

;; Polled by the host while a download runs. phase: idle | checking |
;; downloading | downloaded | error.
(define-record UpdateState
  ([phase : String]
   [percent : Int64]
   [message : (Optional String)]
   [downloaded-path : (Optional String)]
   [available-version : (Optional String)]))

;; Optional slots travel as void on the wire (absent), so callers wrap #f.
(define (nullable value)
  (if value value (void)))
