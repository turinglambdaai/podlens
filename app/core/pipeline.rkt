#lang racket/base

;; The AI pipeline: download → transcribe (ASR) → translate → summarize.
;; Runs inside jobs so the GUI and the CLI share one implementation; the
;; caller only supplies a progress sink.

(require json
         racket/file
         racket/format
         racket/list
         racket/path
         racket/port
         racket/string
         racket/system
         "config.rkt"
         "http.rkt"
         "library.rkt"
         "openai.rkt"
         "paths.rkt"
         "util.rkt")

(provide cfg-snapshot
         ensure-audio!
         run-transcribe!
         run-translate!
         run-summarize!
         make-job-manager
         job-start!
         job-get
         jobs-active?
         has-ffmpeg?
         asr-limit-bytes)

;; ---- config ----------------------------------------------------------------

;; Flattened settings for openai.rkt (symbol keys).
(define (cfg-snapshot mgr)
  (for/hash ([k (in-list (config-keys))])
    (values (string->symbol k) (config-get mgr k))))

;; ---- audio -------------------------------------------------------------------

(define (asr-limit-bytes) (* 24 1024 1024)) ; provider limit is 25 MiB; keep margin

(define (has-ffmpeg?)
  (and (find-executable-path "ffmpeg") #t))

(define (ext-of url)
  (define m (regexp-match #px"\\.([A-Za-z0-9]{2,5})(?:\\?|$)" url))
  (if m (string-downcase (format ".~a" (list-ref m 1))) ".mp3"))

;; Returns a local audio path for the episode, downloading if needed.
;; on-progress: (done total-or-#f)
(define (ensure-audio! episode on-progress)
  (define cached (hash-ref episode 'downloaded-path #f))
  (if (and cached (file-exists? cached))
      cached
      (let* ([id (hash-ref episode 'id)]
             [url (hash-ref episode 'enclosure-url)]
             [dest (audio-path id (ext-of url))])
        (ensure-audio-dir!)
        (define-values (code written)
          (http-download-file url dest on-progress))
        (unless (= code 200)
          (with-handlers ([exn:fail? void]) (delete-file dest))
          (error 'ensure-audio! "download failed (~a): ~a" code url))
        (episode-set-download! id (path->string dest) written)
        (path->string dest))))

;; Split audio into chunks with ffmpeg when it exceeds the ASR limit.
;; Returns (list of paths) — a single-element list when no split was needed.
(define (chunk-audio! cfg audio-path-string)
  (define size (file-size audio-path-string))
  (if (<= size (asr-limit-bytes))
      (list audio-path-string)
      (let ([ffmpeg (find-executable-path "ffmpeg")])
        (unless ffmpeg
          (error 'chunk-audio!
                 "audio is ~a MiB, over the ~a MiB ASR limit; install ffmpeg so PodLens can split it into chunks"
                 (quotient size 1048576)
                 (quotient (asr-limit-bytes) 1048576)))
        (define id (stable-id audio-path-string))
        (define dir (build-path (data-dir) "chunks" id))
        (make-directory* dir)
        (define ext-bytes (path-get-extension audio-path-string))
        (define ext (if ext-bytes (bytes->string/utf-8 ext-bytes) ".mp3"))
        (define pattern (build-path dir (string-append "chunk%03d" ext)))
        (define exit-code
          (system*/exit-code ffmpeg
                             "-y" "-hide_banner" "-loglevel" "error"
                             "-i" audio-path-string
                             "-f" "segment"
                             "-segment_time" (~a (hash-ref cfg 'asr-chunk-seconds 600))
                             "-c" "copy"
                             pattern))
        (unless (= exit-code 0)
          (error 'chunk-audio! "ffmpeg split failed (exit ~a)" exit-code))
        (sort
         (filter (lambda (p) (regexp-match #px"chunk[0-9]+\\." (path->string p)))
                 (directory-list dir #:build? #t))
         string<? #:key path->string))))

;; ---- ASR ---------------------------------------------------------------------

(define (run-transcribe! cfg episode on-progress)
  (define id (hash-ref episode 'id))
  (episode-set-status! id 'transcript-status "running")
  (with-handlers
      ([exn:fail?
        (lambda (e)
          (episode-set-status! id 'transcript-status "error" (exn-message e))
          (raise e))])
    (define audio
      (ensure-audio!
       episode
       (lambda (d t)
         (define pct (inexact->exact (floor (* 30 (/ (min d (max t 1)) (max t 1))))))
         (on-progress (+ 5 pct) "downloading audio"))))
    (on-progress 35 "audio ready")
    (define chunks (chunk-audio! cfg audio))
    (define n-chunks (max 1 (length chunks)))
    (define all-segments '())
    (define language #f)
    (for/fold ([offset 0]
               [i 0])
              ([chunk (in-list chunks)])
      (define result
        (transcribe-file cfg chunk
                         #:prompt "Podcast transcript. Use punctuation and real spelling for names."))
      (set! language (hash-ref result 'language language))
      (define segs (hash-ref result 'segments '()))
      (set! all-segments
            (append all-segments
                    (for/list ([s (in-list segs)])
                      (hasheq 'start (+ offset (hash-ref s 'start 0))
                              'end (+ offset (hash-ref s 'end 0))
                              'text (string-trim (hash-ref s 'text ""))))))
      (on-progress (+ 35 (quotient (* 55 (add1 i)) n-chunks)) "transcribing")
      ;; Next chunk's offset: advance by the last segment's end, or by the
      ;; chunk window when the chunk came back silent (e.g. music) — a silent
      ;; chunk still occupies its segment_time slot in the audio timeline.
      (values
       (+ offset
          (if (null? segs)
              (hash-ref cfg 'asr-chunk-seconds 600)
              (inexact->exact (floor (hash-ref (last segs) 'end 0)))))
       (add1 i)))
    (when (null? all-segments)
      (error 'run-transcribe! "no speech segments recognized"))
    (transcript-save! id
                      (hasheq 'episode-id id
                              'language (or language "")
                              'segments all-segments
                              'translation '()
                              'target-lang ""
                              'summary #f
                              'asr-model (hash-ref cfg 'asr-model)
                              'chat-model (hash-ref cfg 'chat-model)
                              'updated-epoch (now-epoch)))
    (episode-set-status! id 'transcript-status "done")
    (on-progress 100 "transcript ready")
    (length all-segments)))

;; ---- translation ----------------------------------------------------------------

(define (target-name lang)
  (cond
    [(equal? lang "zh") "Simplified Chinese"]
    [(equal? lang "en") "English"]
    [else "English"]))

(define translate-batch-size 25)

(define (run-translate! cfg episode on-progress)
  (define id (hash-ref episode 'id))
  (define t (transcript-load id))
  (unless (and t (pair? (hash-ref t 'segments '())))
    (error 'run-translate! "no transcript for this episode yet — transcribe it first"))
  (episode-set-status! id 'transcript-status "running")
  (with-handlers
      ([exn:fail?
        (lambda (e)
          (episode-set-status! id 'transcript-status "error" (exn-message e))
          (raise e))])
    (define segments (hash-ref t 'segments))
    (define lang (hash-ref cfg 'target-lang))
    (define batches
      (let loop ([xs segments] [acc '()])
        (if (null? xs)
            (reverse acc)
            (let-values ([(take rest) (split-at xs (min translate-batch-size (length xs)))])
              (loop rest (cons take acc))))))
    (define translations
      (for/fold ([acc '()])
                ([batch (in-list batches)]
                 [i (in-naturals)])
        (define numbered
          (string-join
           (for/list ([s (in-list batch)] [n (in-naturals 1)])
             (format "~a. ~a" n (hash-ref s 'text)))
           "\n"))
        (define prompt
          (format "Translate each numbered podcast transcript line into ~a. Keep personal and product names, technical terms and numbers accurate. Match the register of speech, not book language. Reply with JSON: {\"lines\": [\"translation for line 1\", ...]} with exactly ~a entries, same order, no commentary."
                  (target-name lang)
                  (length batch)))
        (define result (chat-completion-json cfg prompt numbered))
        (define lines (hash-ref result 'lines #f))
        (unless (and (list? lines) (= (length lines) (length batch)))
          (error 'run-translate! "translation batch ~a returned wrong line count" (add1 i)))
        (on-progress (quotient (* 100 (add1 i)) (length batches)) "translating")
        (define acc* (append acc (map (lambda (s) (string-trim (format "~a" s))) lines)))
        ;; Persist after every batch: translation is billed per sentence, so a
        ;; failure late in a long episode must not cost the finished batches.
        (transcript-save! id
                          (hash-set* t
                                     'translation acc*
                                     'target-lang lang
                                     'updated-epoch (now-epoch)))
        acc*))
    (transcript-save! id
                      (hash-set* t
                                 'translation translations
                                 'target-lang lang
                                 'updated-epoch (now-epoch)))
    (episode-set-status! id 'transcript-status "done")
    (on-progress 100 "translation ready")
    (length translations)))

;; ---- summary ----------------------------------------------------------------------

(define (run-summarize! cfg episode on-progress)
  (define id (hash-ref episode 'id))
  (define t (transcript-load id))
  (unless (and t (pair? (hash-ref t 'segments '())))
    (error 'run-summarize! "no transcript for this episode yet — transcribe it first"))
  (episode-set-status! id 'summary-status "running")
  (with-handlers
      ([exn:fail?
        (lambda (e)
          (episode-set-status! id 'summary-status "error" (exn-message e))
          (raise e))])
    (on-progress 10 "preparing transcript")
    (define full-text
      (string-join (for/list ([s (in-list (hash-ref t 'segments))]) (hash-ref s 'text)) " "))
    (define max-chars
      (let ([v (hash-ref cfg 'max-transcript-chars 40000)])
        (if (number? v) v 40000)))
    (define text
      (if (<= (string-length full-text) max-chars)
          full-text
          (string-append
           (substring full-text 0 (inexact->exact (floor (* 0.6 max-chars))))
           "\n[… omitted for length …]\n"
           (substring full-text (- (string-length full-text) (inexact->exact (floor (* 0.35 max-chars))))))))
    (define lang (hash-ref cfg 'target-lang))
    (define prompt
      (format "You are a podcast analyst. Read the transcript and produce, in ~a, JSON: {\"tldr\": \"2-3 sentence overview\", \"key_points\": [\"5-8 concrete takeaways\"], \"quotes\": [{{\"text\": \"a short memorable quote in its original language\", \"translation\": \"the same quote in ~a\"}}], \"topics\": [\"3-6 topic tags\"]}. Quote at most 3 quotes. No commentary outside the JSON."
              (target-name lang) (target-name lang)))
    (on-progress 30 "summarizing")
    (define result (chat-completion-json cfg prompt text #:temperature 0.3))
    (unless (and (hash? result) (hash-has-key? result 'tldr))
      (error 'run-summarize! "summary response missing 'tldr'"))
    (transcript-save! id
                      (hash-set* t
                                 'summary (hasheq 'tldr (hash-ref result 'tldr "")
                                                  'key-points (hash-ref result 'key_points '())
                                                  'quotes (hash-ref result 'quotes '())
                                                  'topics (hash-ref result 'topics '()))
                                 'updated-epoch (now-epoch)))
    (episode-set-status! id 'summary-status "done")
    (on-progress 100 "summary ready")
    #t))

;; ---- jobs ---------------------------------------------------------------------------

(struct job-manager (sem jobs emit!))

;; emit! is called as (emit! job-id kind pct message) whenever a running job
;; makes progress or changes state.
(define (make-job-manager emit!)
  (job-manager (make-semaphore 1) (make-hasheq) emit!))

(define (job-set! mgr id patch)
  (with-lock (job-manager-sem mgr)
    (lambda ()
      (hash-update! (job-manager-jobs mgr) id
                    (lambda (j) (for/fold ([j j]) ([k (hash-keys patch)]) (hash-set! j k (hash-ref patch k))))
                    (hasheq)))))

(define (job-start! mgr kind episode-id thunk)
  ;; Check-and-insert runs under the manager lock so two concurrent RPCs
  ;; cannot both start jobs for the same episode (transcribe + translate at
  ;; once would race the episode row and the transcript file).
  (define started
    (with-lock (job-manager-sem mgr)
      (lambda ()
        (define busy?
          (for/or ([j (in-hash-values (job-manager-jobs mgr))])
            (and (equal? (hash-ref j 'episode-id) episode-id)
                 (equal? (hash-ref j 'status) "running"))))
        (if busy?
            #f
            (let* ([id (stable-id (format "~a|~a|~a"
                                          kind episode-id (current-inexact-milliseconds)))]
                   [j (make-hasheq (list (cons 'kind kind)
                                         (cons 'episode-id episode-id)
                                         (cons 'status "running")
                                         (cons 'pct 0)
                                         (cons 'message "")
                                         (cons 'started (now-epoch))))])
              (hash-set! (job-manager-jobs mgr) id j)
              id)))))
  (unless started
    (error 'job-start! "another job is already running for this episode"))
  (define id started)
  (define (progress pct [message ""])
    (job-set! mgr id (hasheq 'pct pct 'message message))
    ((job-manager-emit! mgr) id kind pct message))
  (thread
   (lambda ()
     (with-handlers
         ([exn:fail?
           (lambda (e)
             (job-set! mgr id (hasheq 'status "error" 'message (exn-message e)))
             ((job-manager-emit! mgr) id kind 100 (exn-message e)))])
       (thunk progress)
       (job-set! mgr id (hasheq 'status "done" 'pct 100))
       ((job-manager-emit! mgr) id kind 100 ""))))
  id)

;; status as jsexpr: {id kind episode-id status pct message}
(define (job-get mgr id)
  (define j (hash-ref (job-manager-jobs mgr) id #f))
  (and j
       (hasheq 'id id
               'kind (hash-ref j 'kind)
               'episode-id (hash-ref j 'episode-id)
               'status (hash-ref j 'status)
               'pct (hash-ref j 'pct)
               'message (hash-ref j 'message))))

(define (jobs-active? mgr)
  (for/or ([j (in-hash-values (job-manager-jobs mgr))])
    (equal? (hash-ref j 'status) "running")))
