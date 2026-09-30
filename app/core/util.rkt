#lang racket/base

;; Small shared helpers. Kept dependency-free so every core module and the
;; tests can require it.

(require file/sha1
         json
         racket/file
         racket/format
         racket/port
         racket/string)

(provide with-lock
         bytes->hex
         hex->bytes
         stable-id
         sha256
         read-json-file
         write-json-file!
         trim-or-empty
         now-epoch)

;; Run thunk while holding sem; the post always fires, even on exception.
(define (with-lock sem thunk)
  (dynamic-wind
   (lambda () (semaphore-wait sem))
   thunk
   (lambda () (semaphore-post sem))))

(define (bytes->hex b)
  (apply string-append
         (for/list ([byte (in-bytes b)])
           ;; left-pad so 0x01 renders as "01" (never "10")
           (if (< byte 16)
               (string-append "0" (number->string byte 16))
               (number->string byte 16)))))

(define (hex->bytes s)
  (define n (string-length s))
  (if (and (even? n) (>= n 2))
      (apply bytes
             (for/list ([i (in-range 0 n 2)])
               (string->number (substring s i (+ i 2)) 16)))
      #f))

;; Deterministic id from any string (e.g. feed URL, episode guid+enclosure).
;; file/sha1's sha1 returns the hex digest directly; ids are internal
;; handles, not security tokens.
(define (stable-id s)
  (sha1 (open-input-bytes (string->bytes/utf-8 s))))

(define (read-json-file path)
  (if (file-exists? path)
      (with-handlers ([exn:fail? (lambda (_) #f)])
        (with-input-from-file path read-json))
      #f))

;; Atomic write: temp file + rename, so a crash never truncates the store.
(define (write-json-file! path v)
  (define tmp (format "~a.tmp" (path->string path)))
  (with-output-to-file tmp
    (lambda () (write-json v))
    #:exists 'replace)
  (rename-file-or-directory tmp path #t))

(define (trim-or-empty v)
  (if (string? v) (string-trim v) ""))

(define (now-epoch)
  (current-seconds))

;; ---- SHA-256 (pure Racket) ----------------------------------------------
;;
;; The Racket distribution has no usable SHA-256 without the crypto
;; package, and the embedded runtime does not stage pkgs — so the digest
;; lives here. It is only used on small payloads (manifests, tests); large
;; artifacts are hashed by the native hosts. Verified against FIPS 180-4
;; test vectors in tests/util-test.rkt.

(define sha256-k
  #(#x428a2f98 #x71374491 #xb5c0fbcf #xe9b5dba5 #x3956c25b #x59f111f1 #x923f82a4 #xab1c5ed5
    #xd807aa98 #x12835b01 #x243185be #x550c7dc3 #x72be5d74 #x80deb1fe #x9bdc06a7 #xc19bf174
    #xe49b69c1 #xefbe4786 #x0fc19dc6 #x240ca1cc #x2de92c6f #x4a7484aa #x5cb0a9dc #x76f988da
    #x983e5152 #xa831c66d #xb00327c8 #xbf597fc7 #xc6e00bf3 #xd5a79147 #x06ca6351 #x14292967
    #x27b70a85 #x2e1b2138 #x4d2c6dfc #x53380d13 #x650a7354 #x766a0abb #x81c2c92e #x92722c85
    #xa2bfe8a1 #xa81a664b #xc24b8b70 #xc76c51a3 #xd192e819 #xd6990624 #xf40e3585 #x106aa070
    #x19a4c116 #x1e376c08 #x2748774c #x34b0bcb5 #x391c0cb3 #x4ed8aa4a #x5b9cca4f #x682e6ff3
    #x748f82ee #x78a5636f #x84c87814 #x8cc70208 #x90befffa #xa4506ceb #xbef9a3f7 #xc67178f2))

(define (sha256-u32 x) (bitwise-and x #xFFFFFFFF))

(define (sha256-rotr x n)
  (sha256-u32 (bitwise-ior (arithmetic-shift x (- n)) (arithmetic-shift x (- 32 n)))))

(define (sha256-block! h block)
  (define w (make-vector 64 0))
  (for ([i (in-range 16)])
    (vector-set! w i
                 (for/fold ([acc 0]) ([j (in-range 4)])
                   (bitwise-ior (arithmetic-shift acc 8)
                                (bytes-ref block (+ (* i 4) j))))))
  (for ([i (in-range 16 64)])
    (define s0 (bitwise-xor (sha256-rotr (vector-ref w (- i 15)) 7)
                            (sha256-rotr (vector-ref w (- i 15)) 18)
                            (arithmetic-shift (vector-ref w (- i 15)) -3)))
    (define s1 (bitwise-xor (sha256-rotr (vector-ref w (- i 2)) 17)
                            (sha256-rotr (vector-ref w (- i 2)) 19)
                            (arithmetic-shift (vector-ref w (- i 2)) -10)))
    (vector-set! w i
                 (sha256-u32 (+ (vector-ref w (- i 16)) s0
                                (vector-ref w (- i 7)) s1))))
  (let loop ([a (vector-ref h 0)] [b (vector-ref h 1)] [c (vector-ref h 2)]
             [d (vector-ref h 3)] [e (vector-ref h 4)] [f (vector-ref h 5)]
             [g (vector-ref h 6)] [hh (vector-ref h 7)] [i 0])
    (if (= i 64)
        (begin
          (vector-set! h 0 (sha256-u32 (+ (vector-ref h 0) a)))
          (vector-set! h 1 (sha256-u32 (+ (vector-ref h 1) b)))
          (vector-set! h 2 (sha256-u32 (+ (vector-ref h 2) c)))
          (vector-set! h 3 (sha256-u32 (+ (vector-ref h 3) d)))
          (vector-set! h 4 (sha256-u32 (+ (vector-ref h 4) e)))
          (vector-set! h 5 (sha256-u32 (+ (vector-ref h 5) f)))
          (vector-set! h 6 (sha256-u32 (+ (vector-ref h 6) g)))
          (vector-set! h 7 (sha256-u32 (+ (vector-ref h 7) hh))))
        (let* ([s1 (bitwise-xor (sha256-rotr e 6) (sha256-rotr e 11) (sha256-rotr e 25))]
               [ch (bitwise-xor (bitwise-and e f) (bitwise-and (bitwise-not e) g))]
               [t1 (sha256-u32 (+ hh s1 ch (vector-ref sha256-k i) (vector-ref w i)))]
               [s0 (bitwise-xor (sha256-rotr a 2) (sha256-rotr a 13) (sha256-rotr a 22))]
               [maj (bitwise-xor (bitwise-and a b) (bitwise-and a c) (bitwise-and b c))]
               [t2 (sha256-u32 (+ s0 maj))])
          (loop (sha256-u32 (+ t1 t2)) a b c
                (sha256-u32 (+ d t1)) e f g (add1 i))))))

(define (sha256 data)
  (define len (bytes-length data))
  (define need (+ len 9)) ; payload + 0x80 + 8-byte length, then round up to a block
  (define padded-len (* 64 (+ (quotient need 64) (if (> (remainder need 64) 0) 1 0))))
  (define padded (make-bytes padded-len 0))
  (bytes-copy! padded 0 data)
  (bytes-set! padded len #x80)
  (define bit-len (* 8 len))
  (for ([i (in-range 8)])
    (bytes-set! padded (- padded-len 1 i)
                (bitwise-and (arithmetic-shift bit-len (- (* 8 i))) #xFF)))
  (define h (vector #x6a09e667 #xbb67ae85 #x3c6ef372 #xa54ff53a
                    #x510e527f #x9b05688c #x1f83d9ab #x5be0cd19))
  (let loop ([off 0])
    (when (< off padded-len)
      (define block (make-bytes 64))
      (bytes-copy! block 0 padded off (+ off 64))
      (sha256-block! h block)
      (loop (+ off 64))))
  (apply bytes
         (for/list ([idx (in-range 32)])
           (define word (vector-ref h (quotient idx 4)))
           (bitwise-and (arithmetic-shift word (* 8 (- (remainder idx 4) 3))) #xFF))))
