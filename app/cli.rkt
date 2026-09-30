#lang racket/base

;; PodLens CLI — the same Racket core the native hosts embed, driven from
;; a terminal. Design follows the Taskly CLI conventions:
;;
;;   exit 0 = operation succeeded      exit 1 = operation failed
;;   exit 2 = usage error
;;   --json switches every command to one machine-readable JSON object on
;;   stdout ({"ok":true,...}); progress and errors go to stderr.
;;
;; Run with:  racket app/cli.rkt <command> …   (see `help`)

(require json
         racket/list
         racket/match
         racket/port
         racket/string
         "core/config.rkt"
         "core/feed.rkt"
         "core/i18n.rkt"
         "core/library.rkt"
         "core/paths.rkt"
         "core/pipeline.rkt"
         "core/util.rkt"
         "update.rkt")

(define cli-version "1.0.0")

;; ---- output helpers ----------------------------------------------------------

(define stdout-json? (make-parameter #f))

;; emit the OK json object (fields without "ok") and return 0
(define (emit-ok fields)
  (when (stdout-json?)
    (write-json (hash-set fields 'ok #t))
    (newline))
  0)

(define (fail! code message)
  (if (stdout-json?)
      (begin
        (write-json (hasheq 'ok #f 'error message 'exitCode code))
        (newline))
      (begin
        (eprintf "error: ~a\n" message)
        (flush-output (current-error-port))))
  code)

(define (usage! [message #f])
  (when message
    (if (stdout-json?)
        (begin
          (write-json (hasheq 'ok #f 'error message 'exitCode 2))
          (newline))
        (begin
          (eprintf "error: ~a\n" message)
          (flush-output (current-error-port)))))
  2)

(define (progress->stderr label)
  (lambda (pct message)
    (when (stdout-json?)
      (eprintf "~a ~a%\n" label pct)
      (flush-output (current-error-port)))))

;; ---- context -----------------------------------------------------------------

(define cli-config (make-parameter #f))

(define (config!) (or (cli-config) (make-config-manager)))

(define (lang) (config-get (config!) 'ui-lang))

;; episode lookup or a failed run
(struct stopped (code))

(define (find-episode-or-stop id)
  (cond
    [(episode-get id) => values]
    [else (stopped 1)]))

(define (run-command command-name args command-body)
  command-body)

;; ---- commands ----------------------------------------------------------------

(define (cmd-add args)
  (cond
    [(not (= 1 (length args)))
     (usage! (tr (lang) 'usage))]
    [else
     (with-handlers ([exn:fail? (lambda (e) (fail! 1 (exn-message e)))])
       (define parsed (fetch-feed (string-trim (car args))))
       (define id (feed-add! (string-trim (car args)) parsed))
       (if (stdout-json?)
           (emit-ok (hasheq 'feedId id 'title (hash-ref parsed 'title)))
           (displayln (tr (lang) 'added)))
       0)]))

(define (cmd-list _args)
  (define feeds (feed-all))
  (cond
    [(stdout-json?)
     (emit-ok
      (hasheq 'feeds
              (for/list ([f (in-list feeds)])
                (hasheq 'id (hash-ref f 'id)
                        'title (hash-ref f 'title)
                        'author (hash-ref f 'author)
                        'url (hash-ref f 'url)
                        'episodeCount (length (episodes-for-feed (hash-ref f 'id)))))))]
    [else
     (when (null? feeds) (displayln (tr (lang) 'no-feeds)))
     (for ([f (in-list feeds)])
       (displayln (format "~a  ~a  (~a)"
                          (substring (hash-ref f 'id) 0 12)
                          (hash-ref f 'title)
                          (hash-ref f 'url))))
     0]))

(define (cmd-episodes args)
  (cond
    [(not (= 1 (length args))) (usage! (tr (lang) 'usage))]
    [(not (feed-get (car args))) (usage! (tr (lang) 'no-feed))]
    [else
     (define eps (episodes-for-feed (car args)))
     (if (stdout-json?)
         (emit-ok
          (hasheq 'episodes
                  (for/list ([e (in-list eps)])
                    (hasheq 'id (hash-ref e 'id)
                            'title (hash-ref e 'title)
                            'pubDate (hash-ref e 'pub-date-display)
                            'durationSec (hash-ref e 'duration-sec #f)
                            'downloaded (hash-ref e 'downloaded-path #f)
                            'transcript (hash-ref e 'transcript-status "none")
                            'positionSec (hash-ref e 'position-sec 0)))))
         (begin
           (for ([e (in-list eps)])
             (displayln (format "~a  ~a  ~a"
                                (substring (hash-ref e 'id) 0 12)
                                (hash-ref e 'title)
                                (hash-ref e 'pub-date-display))))
           0))]))

;; shared body for the three transcript-pipeline commands
(define (with-episode id body)
  (cond
    [(not (= 1 (length (list id)))) (usage! (tr (lang) 'usage))]
    [else
     (define e (episode-get id))
     (cond
       [(not e) (fail! 1 (tr (lang) 'no-episode))]
       [else
        (with-handlers ([exn:fail? (lambda (err) (fail! 1 (exn-message err)))])
          (body e))])]))

(define (cmd-download args)
  (cond
    [(not (= 1 (length args))) (usage! (tr (lang) 'usage))]
    [else
     (with-episode
      (car args)
      (lambda (e)
        (define path (ensure-audio! e (progress->stderr "download")))
        (if (stdout-json?)
            (emit-ok (hasheq 'path path))
            (begin (displayln (tr (lang) 'downloaded)) 0))
        (if (stdout-json?) 0 (void))
        0))]))

(define (cmd-transcribe args)
  (cond
    [(not (= 1 (length args))) (usage! (tr (lang) 'usage))]
    [else
     (with-episode
      (car args)
      (lambda (e)
        (define n (run-transcribe! (cfg-snapshot (config!)) e (progress->stderr "transcribe")))
        (if (stdout-json?)
            (emit-ok (hasheq 'segments n))
            (displayln (tr (lang) 'transcribed n)))
        0))]))

(define (cmd-translate args)
  (cond
    [(not (= 1 (length args))) (usage! (tr (lang) 'usage))]
    [else
     (with-episode
      (car args)
      (lambda (e)
        (define n (run-translate! (cfg-snapshot (config!)) e (progress->stderr "translate")))
        (if (stdout-json?)
            (emit-ok (hasheq 'segments n))
            (displayln (tr (lang) 'translated n)))
        0))]))

(define (cmd-summarize args)
  (cond
    [(not (= 1 (length args))) (usage! (tr (lang) 'usage))]
    [else
     (with-episode
      (car args)
      (lambda (e)
        (run-summarize! (cfg-snapshot (config!)) e (progress->stderr "summarize"))
        (if (stdout-json?)
            (emit-ok (hasheq 'summarized #t))
            (displayln (tr (lang) 'summarized)))
        0))]))

(define (cmd-show args)
  (cond
    [(not (= 1 (length args))) (usage! (tr (lang) 'usage))]
    [else
     (define id (car args))
     (define e (episode-get id))
     (cond
       [(not e) (fail! 1 (tr (lang) 'no-episode))]
       [else
        (define t (transcript-load id))
        (if (stdout-json?)
            (begin
              (write-json
               (hasheq 'ok #t
                       'episode (hasheq 'id id 'title (hash-ref e 'title))
                       'transcript
                       (if t
                           (for/list ([s (in-list (hash-ref t 'segments '()))])
                             (hasheq 'start (hash-ref s 'start)
                                     'end (hash-ref s 'end)
                                     'text (hash-ref s 'text)))
                           '())
                       'summary (and t (hash-ref t 'summary #f))))
              (newline)
              0)
            (begin
              (when t
                (for ([s (in-list (hash-ref t 'segments '()))])
                  (displayln (format "[~a → ~a] ~a"
                                     (hash-ref s 'start)
                                     (hash-ref s 'end)
                                     (hash-ref s 'text)))))
              (when (and t (hash-ref t 'summary #f))
                (displayln (hash-ref (hash-ref t 'summary) 'tldr "")))
              0))])]))

(define (cmd-position args)
  (cond
    [(not (= 2 (length args))) (usage! (tr (lang) 'usage))]
    [else
     (define id (car args))
     (define e (episode-get id))
     (cond
       [(not e) (fail! 1 (tr (lang) 'no-episode))]
       [else
        (episode-set-position! id (or (string->number (cadr args)) 0))
        (if (stdout-json?)
            (emit-ok (hasheq 'positionSec (or (string->number (cadr args)) 0)))
            (displayln (tr (lang) 'position-saved)))
        0])]))

(define (cmd-refresh args)
  (with-handlers ([exn:fail? (lambda (e) (fail! 1 (exn-message e)))])
    (define ids (if (null? args) (map (lambda (f) (hash-ref f 'id)) (feed-all)) args))
    (define total
      (for/sum ([id (in-list ids)])
        (define f (feed-get id))
        (unless f (error 'refresh (tr (lang) 'no-feed)))
        (define before (length (episodes-for-feed id)))
        (feed-add! (hash-ref f 'url) (fetch-feed (hash-ref f 'url)))
        (- (length (episodes-for-feed id)) before)))
    (if (stdout-json?)
        (emit-ok (hasheq 'newEpisodes total))
        (displayln (tr (lang) 'refreshed total)))
    0))

(define (cmd-config args)
  (define c (config!))
  (match args
    [(list)
     (if (stdout-json?)
         (emit-ok
          (hasheq 'config
                  (for/hasheq ([k (in-list (config-keys))])
                    (values (string->symbol k) (config-get c k)))))
         (begin
           (for ([k (in-list (config-keys))])
             (displayln (format "~a = ~a" k (config-get c k))))
           0))]
    [(list key)
     (cond
       [(not (member key (config-keys))) (usage! (tr (lang) 'unknown-key key))]
       [(stdout-json?) (emit-ok (hasheq (string->symbol key) (config-get c key)))]
       [else (displayln (config-get c key)) 0])]
    [(list key value)
     (with-handlers ([exn:fail? (lambda (e) (fail! 2 (exn-message e)))])
       (unless (member key (config-keys))
         (error 'config (tr (lang) 'unknown-key key)))
       (define v (config-set! c key value))
       (if (stdout-json?)
           (emit-ok (hasheq (string->symbol key) v))
           (displayln (tr (lang) 'config-set key (format "~a" v))))
       0)]
    [_ (usage! (tr (lang) 'usage))]))

(define (cmd-check-updates _args)
  (with-handlers ([exn:fail? void])
    (define status (update-check cli-version))
    (if (stdout-json?)
        (emit-ok (hasheq 'status status))
        (displayln status))
    0))

(define (cmd-doctor _args)
  (ensure-data-dir!)
  (define data-ok?
    (with-handlers ([exn:fail? (lambda (_) #f)])
      (define probe (build-path (data-dir) ".probe"))
      (with-output-to-file probe (lambda () (display "x")) #:exists 'replace)
      (delete-file probe)
      #t))
  (define api-ok? (config-api-configured? (config!)))
  (define ffmpeg-ok? (has-ffmpeg?))
  (if (stdout-json?)
      (emit-ok
       (hasheq 'dataDir (path->string (data-dir))
               'dataWritable data-ok?
               'apiConfigured api-ok?
               'ffmpeg ffmpeg-ok?))
      (begin
        (displayln (format "data-dir:   ~a — ~a" (path->string (data-dir)) (tr (lang) 'doctor-data)))
        (displayln (format "api:        ~a" (if api-ok? (tr (lang) 'doctor-api) (tr (lang) 'doctor-no-api))))
        (displayln (format "ffmpeg:     ~a" (if ffmpeg-ok? (tr (lang) 'doctor-ffmpeg) (tr (lang) 'doctor-no-ffmpeg))))))
  (if data-ok? 0 1))

(define (cmd-version _args)
  (if (stdout-json?) (emit-ok (hasheq 'version cli-version)) (displayln cli-version))
  0)

(define help-text
  (string-append
   "PodLens " cli-version " — podcasts, in your language\n"
   "\n"
   "  add <rss-url>              subscribe to a podcast feed\n"
   "  list                       show subscriptions\n"
   "  episodes <feed-id>         list episodes of a feed\n"
   "  refresh [feed-id]…         refresh feeds for new episodes\n"
   "  download <episode-id>      cache the episode audio\n"
   "  transcribe <episode-id>    speech-to-text via the ASR API\n"
   "  translate <episode-id>     translate the transcript\n"
   "  summarize <episode-id>     TL;DR + key points + quotes\n"
   "  show <episode-id>          print transcript and summary\n"
   "  position <episode-id> <s>  remember a playback position\n"
   "  config [key [value]]       read or set settings\n"
   "  check-updates              signed update check\n"
   "  doctor                     health check\n"
   "  --json                     machine-readable output\n"
   "\n"
   "Exit codes: 0 ok, 1 operation failed, 2 usage error.\n"
   "Data lives in ~/.podlens (override with PODLENS_DATA_DIR).\n"))

(define (cmd-help _args)
  (display help-text)
  0)

;; ---- dispatch ------------------------------------------------------------------

(define commands
  (hasheq 'add cmd-add
          'list cmd-list
          'episodes cmd-episodes
          'refresh cmd-refresh
          'download cmd-download
          'transcribe cmd-transcribe
          'translate cmd-translate
          'summarize cmd-summarize
          'show cmd-show
          'position cmd-position
          'config cmd-config
          'check-updates cmd-check-updates
          'doctor cmd-doctor
          'version cmd-version
          'help cmd-help))

(module+ main
  (require racket/match)
  (define raw-args (vector->list (current-command-line-arguments)))
  (define args (filter (lambda (a) (not (equal? a "--json"))) raw-args))
  (stdout-json? (not (equal? (length args) (length raw-args))))
  (ensure-data-dir!)
  (cli-config (make-config-manager))
  (library-load!)
  (exit
   (with-handlers
       ([exn:fail? (lambda (e) (fail! 1 (exn-message e)))])
     (match args
       ['() (cmd-help '())]
       [(list (or "help" "-h" "--help" "version" "--version")) ((hash-ref commands (if (member (car args) '("version" "--version")) 'version 'help) cmd-help) (cdr args))]
       [(cons cmd rest)
        (define command (hash-ref commands (string->symbol cmd) #f))
        (cond
          [command (command rest)]
          [else (usage! (tr (lang) 'usage))])]
       [_ (usage! (tr (lang) 'usage))]))))
