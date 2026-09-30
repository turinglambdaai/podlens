#lang racket/base

;; SemVer-ish comparison for update checks. Dot segments compare
;; numerically; missing segments count as 0 (1.2 > 1.1.9). Pre-release
;;/build metadata are ignored for the v1 check (stable channel only).

(require racket/list
         racket/string)

(provide version->segments
         version<?
         version>?
         version=?)

(define (version->segments v)
  (for/list ([part (in-list (string-split (string-trim v) "."))])
    (define n (string->number part))
    (or n 0)))

(define (pad a b)
  (define n (max (length a) (length b)))
  (define (pad1 xs)
    (append xs (make-list (- n (length xs)) 0)))
  (values (pad1 a) (pad1 b)))

(define (version-compare a b)
  (define-values (as bs) (pad (version->segments a) (version->segments b)))
  (for/or ([x (in-list as)] [y (in-list bs)]) (- x y)))

(define (version<? a b) (< (version-compare a b) 0))
(define (version>? a b) (> (version-compare a b) 0))
(define (version=? a b) (= (version-compare a b) 0))
