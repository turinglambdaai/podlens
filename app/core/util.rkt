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
