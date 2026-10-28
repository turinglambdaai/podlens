#lang racket/base

;; Deterministic offline data for the native listening-journey acceptance.
;; No account, API key, real feed, or downloaded media is involved.

(require json
         racket/file
         racket/path
         (file "../../app/core/library.rkt")
         (file "../../app/core/paths.rkt"))

(define parsed
  (hasheq
   'title "PodLens Acceptance Radio"
   'author "PodLens"
   'description "Synthetic offline fixture for the listening journey."
   'artwork-url ""
   'items
   (list
    (hasheq 'guid "listening-journey-1"
            'title "A small episode about focused listening"
            'pub-date-epoch 1
            'pub-date-display "2026-10-25"
            'enclosure-url "https://fixtures.invalid/focused-listening.wav"
            'enclosure-length 128044
            'enclosure-type "audio/wav"
            'duration-sec 8
            'chapters-url ""
            'description "Synthetic audio and bilingual transcript."))))

(define (le value size)
  (integer->integer-bytes value size #f #f))

(define (silent-wav seconds)
  (define sample-rate 8000)
  (define channels 1)
  (define bits-per-sample 16)
  (define block-align (* channels (quotient bits-per-sample 8)))
  (define byte-rate (* sample-rate block-align))
  (define data-size (* seconds byte-rate))
  (bytes-append
   #"RIFF" (le (+ 36 data-size) 4) #"WAVE"
   #"fmt " (le 16 4) (le 1 2) (le channels 2) (le sample-rate 4)
   (le byte-rate 4) (le block-align 2) (le bits-per-sample 2)
   #"data" (le data-size 4) (make-bytes data-size 0)))

(library-load!)
(define feed-id (feed-add! "https://fixtures.invalid/podlens.xml" parsed))
(define episode (car (episodes-for-feed feed-id)))
(define episode-id (hash-ref episode 'id))
(define audio-dir (build-path (data-dir) "audio"))
(define audio-path (build-path audio-dir (string-append episode-id ".wav")))
(make-directory* audio-dir)
(call-with-output-file audio-path
  (lambda (out) (void (write-bytes (silent-wav 8) out)))
  #:exists 'truncate/replace
  #:mode 'binary)

(transcript-save!
 episode-id
 (hasheq
  'episode-id episode-id
  'language "en"
  'segments
  (list (hasheq 'start 0 'end 2 'text "The best player gets out of the way.")
        (hasheq 'start 2 'end 4 'text "A transcript should stay close to the audio.")
        (hasheq 'start 4 'end 6 'text "Keyboard listeners deserve the same direct path.")
        (hasheq 'start 6 'end 8 'text "Return to the current sentence when you are ready."))
  'translation
  (list "最好的播放器会隐于内容之后。"
        "逐句稿应始终贴近声音。"
        "键盘用户也应拥有同样直接的路径。"
        "准备好后，回到当前句。")
  'target-lang "zh"
  'summary #f
  'asr-model "fixture"
  'chat-model "fixture"
  'updated-epoch 1))
(void (episode-set-download! episode-id (path->string audio-path) (file-size audio-path)))
(void (episode-set-status! episode-id 'transcript-status "done"))
(void (episode-set-position! episode-id 1))

(call-with-output-file (build-path (data-dir) "listening-fixture.json")
  (lambda (out)
    (write-json (hasheq 'feed-id feed-id
                        'episode-id episode-id
                        'seek-seconds 4)
                out))
  #:exists 'truncate/replace)
