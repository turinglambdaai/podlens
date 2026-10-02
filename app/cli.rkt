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
         racket/format
         racket/list
         racket/match
         racket/port
         racket/set
         racket/string
         "core/catalog.rkt"
         "core/config.rkt"
         "core/feed.rkt"
         "core/i18n.rkt"
         "core/itunes.rkt"
         "core/library.rkt"
         "core/paths.rkt"
         "core/pipeline.rkt"
         "core/util.rkt"
         "update.rkt"
         "version.rkt")

;; the version mirrors rivet.rktd (see app/version.rkt)
(define cli-version app-version)

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

(define (cmd-pipeline args)
  ;; one-click "understand this episode": download → transcribe → translate
  ;; → summarize; finished stages are skipped, so a rerun is cheap
  (cond
    [(not (= 1 (length args))) (usage! (tr (lang) 'usage))]
    [else
     (with-episode
      (car args)
      (lambda (e)
        (define stages
          (run-pipeline! (cfg-snapshot (config!)) e (progress->stderr "pipeline")))
        (if (stdout-json?)
            (emit-ok (hasheq 'stages stages))
            (begin
              (displayln (tr (lang) 'pipelined))
              (for ([s (in-list stages)])
                (displayln (format "  ✓ ~a" s)))))
        0))]))

(define (cmd-estimate args)
  ;; what a pipeline run would cost — sentence/char counts before committing
  (cond
    [(not (= 1 (length args))) (usage! (tr (lang) 'usage))]
    [else
     (define e (episode-get (car args)))
     (cond
       [(not e) (fail! 1 (tr (lang) 'no-episode))]
       [else
        (define est (estimate-episode (cfg-snapshot (config!)) e))
        (if (stdout-json?)
            (emit-ok (hasheq 'estimate est))
            (begin
              (displayln (format "duration-sec: ~a" (hash-ref est 'durationSec)))
              (displayln (format "transcript:   ~a"
                                 (if (hash-ref est 'transcriptKnown) "yes" "no")))
              (displayln (format "sentences:    ~a" (hash-ref est 'sentences)))
              (displayln (format "chars:        ~a" (hash-ref est 'chars)))
              (displayln (format "translated:   ~a" (hash-ref est 'translated)))
              (displayln (format "summarized:   ~a" (hash-ref est 'summarized)))
              0))])]))

(define (cmd-done args)
  ;; mark played / unplayed; unplayed resets the position
  (cond
    [(or (< (length args) 1) (> (length args) 2)) (usage! (tr (lang) 'usage))]
    [else
     (define id (car args))
     (define done?
       (if (= (length args) 2)
           (member (cadr args) '("1" "true" "yes"))
           #t))
     (cond
       [(not (episode-get id)) (fail! 1 (tr (lang) 'no-episode))]
       [else
        (episode-set-done! id done?)
        (unless done? (episode-set-position! id 0))
        (if (stdout-json?)
            (emit-ok (hasheq 'id id 'done done?))
            (displayln (if done? (tr (lang) 'marked-done) (tr (lang) 'marked-undone))))
        0])]))

(define (cmd-export args)
  ;; timestamped transcript (+ summary) as Markdown — the citation format
  ;; agents paste into notes
  (cond
    [(or (< (length args) 1) (> (length args) 2)) (usage! (tr (lang) 'usage))]
    [else
     (define id (car args))
     (define e (episode-get id))
     (cond
       [(not e) (fail! 1 (tr (lang) 'no-episode))]
       [else
        (define t (transcript-load id))
        (cond
          [(not (and t (pair? (hash-ref t 'segments '()))))
           (fail! 1 (tr (lang) 'export-no-transcript))]
          [else
           (define feed (feed-get (hash-ref e 'feed-id)))
           (define md (export-markdown e feed t))
           (define path
             (if (= (length args) 2)
                 (cadr args)
                 (format "podlens-~a.md" (substring id 0 12))))
           (with-output-to-file path (lambda () (display md)) #:exists 'replace)
           (if (stdout-json?)
               (emit-ok (hasheq 'path path))
               (displayln (tr (lang) 'exported path)))
           0])])]))

(define (mmss sec)
  (define s (inexact->exact (floor sec)))
  (format "~a:~a" (quotient s 60) (~r (remainder s 60) #:min-width 2 #:pad-string "0")))

(define (export-markdown e feed t)
  (define summary (hash-ref t 'summary #f))
  (string-join
   (append
    (list (format "# ~a" (hash-ref e 'title ""))
          ""
          (format "- Podcast: ~a" (if feed (hash-ref feed 'title "") ""))
          (format "- Published: ~a" (hash-ref e 'pub-date-display ""))
          "")
    (if summary
        (append
         (list "## TL;DR" "" (hash-ref summary 'tldr "") "")
         (if (pair? (hash-ref summary 'key-points '()))
             (append (list "## Key points" "")
                     (for/list ([p (in-list (hash-ref summary 'key-points))])
                       (format "- ~a" p))
                     (list ""))
             '())
         (if (pair? (hash-ref summary 'quotes '()))
             (append (list "## Notable quotes" "")
                     (for/list ([q (in-list (hash-ref summary 'quotes))])
                       (format "> ~a~a"
                               (hash-ref q 'text "")
                               (let ([tr-q (hash-ref q 'translation "")])
                                 (if (blank-string? tr-q) "" (format "\n> — ~a" tr-q)))))
                     (list ""))
             '())
         (if (pair? (hash-ref summary 'topics '()))
             (append (list "## Topics" ""
                           (string-join
                            (for/list ([tp (in-list (hash-ref summary 'topics))])
                              (format "`~a`" tp))
                            " · ")
                           "")
                     (list ""))
             '()))
        '())
    (list "## Transcript" "")
    (append*
     (let ([trans (hash-ref t 'translation '())])
       (for/list ([s (in-list (hash-ref t 'segments '()))] [i (in-naturals)])
         (define tr-line (if (< i (length trans)) (list-ref trans i) ""))
         (list (format "- `~a` ~a" (mmss (hash-ref s 'start 0)) (hash-ref s 'text ""))
               (if (blank-string? tr-line) "" (format "  ~a" tr-line))))))
    (list ""))
   "\n"))

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

(define (cmd-catalog args)
  ;; list the curated catalog; mark entries already subscribed
  (define subscribed-urls
    (list->set (map (lambda (f) (string-trim (hash-ref f 'url ""))) (feed-all))))
  (define wanted (if (null? args) #f (car args)))
  (define rows
    (filter (lambda (r) (or (not wanted) (equal? (car r) wanted)))
            (catalog-entries)))
  (if (stdout-json?)
      (emit-ok
       (hasheq 'catalog
               (for/list ([e (in-list rows)])
                 (hasheq 'id (catalog-entry-id e)
                         'category (catalog-entry-category e)
                         'name (catalog-entry-name e)
                         'url (catalog-entry-url e)
                         'added (if (set-member? subscribed-urls (catalog-entry-url e)) #t #f)))))
      (begin
        (for ([e (in-list rows)])
          (displayln (format "~a  [~a]~a  ~a"
                             (catalog-entry-id e)
                             (catalog-entry-category e)
                             (if (set-member? subscribed-urls (catalog-entry-url e)) " ✓" "")
                             (catalog-entry-name e)))
          (displayln (format "    ~a" (catalog-entry-url e))))
        0)))

(define (cmd-search args)
  ;; search the full podcast directory (iTunes Search API); add results
  ;; through the normal `add <url>`
  (cond
    [(null? args) (usage! (tr (lang) 'usage))]
    [else
     (with-handlers ([exn:fail? (lambda (e) (fail! 1 (exn-message e)))])
       (define query (string-join args))
       (define rows
         (itunes-search-podcasts query (config-number (config!) 'search-limit)))
       (if (stdout-json?)
           (emit-ok
            (hasheq 'results
                    (for/list ([r (in-list rows)])
                      (hasheq 'id (hash-ref r 'id)
                              'title (hash-ref r 'title)
                              'artist (hash-ref r 'artist)
                              'genre (hash-ref r 'genre)
                              'feedUrl (hash-ref r 'feed-url)
                              'homepage (hash-ref r 'homepage)))))
           (begin
             (when (null? rows) (displayln (tr (lang) 'no-results)))
             (for ([r (in-list rows)])
               (displayln (format "~a  [~a]  ~a"
                                  (hash-ref r 'title)
                                  (hash-ref r 'genre)
                                  (hash-ref r 'artist)))
               (displayln (format "    ~a" (hash-ref r 'feed-url))))
             0)))]))

(define (cmd-find args)
  ;; full-text search across every subscription's transcripts; answers
  ;; "what did they say about X, at which minute" with sentence timestamps
  (cond
    [(null? args) (usage! (tr (lang) 'usage))]
    [else
     (define terms
       (for/list ([a (in-list args)])
         (string-downcase (string-trim a))))
     (define matches
       (for*/list ([e (in-list (episode-all))]
                   [t (in-value (transcript-load (hash-ref e 'id)))]
                   #:when t
                   [s (in-list (hash-ref t 'segments '()))]
                   #:do [(define text
                           (string-downcase (hash-ref s 'text "")))
                         (define hit?
                           (for/and ([term (in-list terms)])
                             (string-contains? text term)))]
                   #:when hit?)
         (hasheq 'episodeId (hash-ref e 'id)
                 'episodeTitle (hash-ref e 'title "")
                 'start (hash-ref s 'start 0)
                 'end (hash-ref s 'end 0)
                 'text (hash-ref s 'text ""))))
     (define limited (take matches (min (length matches) 200)))
     (if (stdout-json?)
         (emit-ok (hasheq 'total (length matches) 'matches limited))
         (begin
           (when (null? matches) (displayln (tr (lang) 'no-results)))
           (for ([m (in-list limited)])
             (displayln (format "~a  ~a  ~a"
                                (mmss (hash-ref m 'start))
                                (substring (hash-ref m 'episodeId) 0 12)
                                (hash-ref m 'text))))
           0))]))

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
  (with-handlers ([exn:fail? (lambda (e) (fail! 1 (exn-message e)))])
    (define status
      (match (update-check-result cli-version)
        [(list 'available v) (tr (lang) 'update-available v)]
        [(list 'up-to-date) (tr (lang) 'up-to-date)]
        [(list 'unavailable _) (tr (lang) 'update-dev)]
        [(list 'failed m) (tr (lang) 'update-failed m)]
        [_ (tr (lang) 'update-failed "unknown")]))
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
   "  catalog [category]         list the curated catalog\n"
   "  search <terms>             search the full podcast directory\n"
   "  download <episode-id>      cache the episode audio\n"
   "  pipeline <episode-id>      download+transcribe+translate+summarize\n"
   "  estimate <episode-id>      what a pipeline run would cost\n"
   "  transcribe <episode-id>    speech-to-text via the ASR API\n"
   "  translate <episode-id>     translate the transcript\n"
   "  summarize <episode-id>     TL;DR + key points + quotes\n"
   "  show <episode-id>          print transcript and summary\n"
   "  export <episode-id> [file] transcript+summary as Markdown (@mm:ss)\n"
   "  find <terms>…              full-text search across transcripts\n"
   "  done <episode-id> [0|1]    mark played (1, default) or unplayed (0)\n"
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
          'catalog cmd-catalog
          'search cmd-search
          'download cmd-download
          'pipeline cmd-pipeline
          'estimate cmd-estimate
          'transcribe cmd-transcribe
          'translate cmd-translate
          'summarize cmd-summarize
          'show cmd-show
          'export cmd-export
          'find cmd-find
          'done cmd-done
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
