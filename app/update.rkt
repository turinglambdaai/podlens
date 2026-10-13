#lang racket/base

;; Online update support for PodLens, built on rivet/distribution — the
;; family pattern (taskly/brainfuel/payback/syncpilot): the backend
;; verifies and downloads the signed update artifact; the native host owns
;; installation. A check fetches and verifies the Ed25519-signed channel
;; manifest (the signed wrapper in docs/UPDATE.md); the artifact download
;; runs on a background thread with progress published to a state box that
;; hosts poll through the `update-state` RPC (RVT1 events are thread-local,
;; so a background thread cannot emit them directly).
;;
;; PodLens-specific rules (docs/UPDATE.md): the 4-hour silent-check
;; throttle lives in the hosts (macOS UserDefaults / Windows config key) —
;; this module only persists the sticky rollout bucket. Downloads land
;; under <data-dir>/updates/ and config.json / library.json / audio are
;; never touched.
;;
;; Note on crypto factories: rivet/distribution pins the provider set to
;; libcrypto at module load and deliberately avoids crypto/all — factory
;; FFIs load at import time and the gmp factory kills embedded runtimes on
;; hosts without libgmp. This module runs inside the embedded CS runtime,
;; so it must not require crypto/all either (tests may, they are headless).
;; This replaces the earlier hand-rolled OpenSSL 3 subprocess scheme: the
;; crypto package's libcrypto factory does the same Ed25519/SHA-256 work
;; in-process, with no PATH dependence and no temp-file dance.

(require crypto
         json
         net/base64
         rivet/distribution
         racket/file
         racket/list
         racket/path
         racket/port
         racket/string
         "core/paths.rkt"
         "version.rkt")

(provide app-version
         app-build
         app-identifier
         app-channel
         app-display-name
         update-key-id
         update-public-key-b64
         default-update-base-url
         current-update-base-url
         releases-page
         update-configured?
         platform-symbol
         architecture-symbol
         installer-extension
         manifest-url
         destination-path
         update-state-snapshot
         reset-update-state!
         set-update-error!
         current-candidate
         set-candidate!
         set-update-data-dir-override!
         perform-check!
         start-download!
         rollout-bucket)

;; Release identity: the version comes from core/version.rkt (the single
;; source scripts/check-release-version.sh pins to VERSION/rivet.rktd);
;; the rest duplicates rivet.rktd, which the packaged app cannot read at
;; runtime.
(define app-build 1)
(define app-identifier "site.jrtx.podlens")
(define app-channel 'stable)
(define app-display-name "PodLens")

(define update-key-id "podlens-2026-10")

;; Ed25519 public key, base64 of the raw 32 bytes (scripts/update-keys.sh)
;; — the same literal the macOS host embeds (UpdateService.swift). The
;; private half lives outside the repository and never ships. Rotate by
;; shipping a build that trusts the next key before signing releases
;; exclusively with it.
(define update-public-key-b64
  "CRJhkEfo79/a7lgRkLhWSvVGviGcLNL2obvE2k7bFZI=")

;; Where the updater looks for the signed manifest. The release pipeline
;; pins artifact URLs to the concrete tag; the manifest itself is always
;; fetched from the moving "latest" location. Tests parameterize this at
;; an unreachable local URL instead of touching the network.
(define default-update-base-url
  "https://github.com/turinglambdaai/podlens/releases/latest/download")

(define current-update-base-url (make-parameter #f))

;; Human-facing releases page; the open-releases RPC sends hosts here.
(define releases-page
  "https://github.com/turinglambdaai/podlens/releases/latest")

(define maximum-download-bytes (* 800 1024 1024))

;; ---------- public key ----------

;; Fixed SPKI prefix bytes for an Ed25519 public key (12 bytes) followed by
;; the raw 32-byte key = full SubjectPublicKeyInfo DER. (hex->bytes comes
;; from the crypto require.)
(define ed25519-spki-prefix (hex->bytes "302a300506032b6570032100"))

(define (update-configured?)
  (and (string? update-public-key-b64)
       (let ([raw (base64-string->bytes update-public-key-b64)])
         (and (= 32 (bytes-length raw)) (embedded-public-key) #t))))

(define (embedded-public-key)
  (datum->pk-key
   (bytes-append ed25519-spki-prefix
                 (base64-string->bytes update-public-key-b64))
   'SubjectPublicKeyInfo))

;; ---------- platform identity ----------

;; rivet release tooling emits these exact symbols into update manifests
(define (platform-symbol)
  (case (system-type 'os)
    [(macosx) 'macos]
    [(windows) 'windows]
    [else 'linux]))

(define (architecture-symbol)
  (case (system-type 'arch)
    [(aarch64 arm64) 'arm64]
    [else 'x64]))

(define (installer-extension)
  (case (system-type 'os)
    [(macosx) ".zip"]
    [(windows) ".zip"]
    [else ".tar.gz"]))

(define (manifest-url)
  (string-append
   (string-trim (or (current-update-base-url) default-update-base-url)
                "/" #:right? #t)
   "/update-manifest.json"))

;; ---------- shared update state (UI-visible) ----------

;; phase: idle | checking | downloading | downloaded | error
(define update-state
  (box (hasheq 'phase "idle"
               'percent 0
               'message #f
               'downloadedPath #f
               'availableVersion #f)))

(define candidate-box (box #f))
(define worker-thread-box (box #f))

;; Download watchdog bounds. rivet's copy loop waits on read-bytes-avail!
;; with no timeout, so a connection that stalls without RST/FIN parks the
;; worker thread forever — visibly "stuck at 100%" (all bytes had arrived;
;; the loop was still waiting for EOF). Two finite bounds keep the phase
;; machine honest: no byte progress for the stall limit, or wall clock
;; past the total limit, breaks the worker (rivet's partial-file cleanup
;; runs, and the worker reports phase=error itself) so the host always
;; gets a terminal state.
(define download-stall-limit-seconds 120)
(define download-total-limit-seconds (* 30 60))
(define last-progress-seconds (box 0))

(define (state-set! key value)
  (set-box! update-state (hash-set (unbox update-state) key value)))

(define (update-state-snapshot)
  (unbox update-state))

(define (reset-update-state!)
  (set-box! candidate-box #f)
  (set-box! update-state
            (hasheq 'phase "idle"
                    'percent 0
                    'message #f
                    'downloadedPath #f
                    'availableVersion #f)))

;; Pre-spawn download failures (already running, no candidate) surface
;; through the state instead of an RPC error, so host UIs have a single
;; failure channel.
(define (set-update-error! message)
  (state-set! 'phase "error")
  (state-set! 'message message))

;; The candidate stashed by the last successful check: an update-candidate
;; struct or #f. The backend maps it onto the UpdateCheck record's
;; optional fields.
(define (current-candidate) (unbox candidate-box))

;; Test seam: install a candidate without a network check so the download
;; can be driven against a local HTTP server.
(define (set-candidate! candidate) (set-box! candidate-box candidate))

;; ---------- updater state (rollout bucket persistence) ----------

;; Exactly these keys may exist in updater-state.json — the updater's own
;; tiny store, kept out of the user-facing config.json schema. Values are
;; integers (0-99 for the bucket).
(define updater-state-keys '(rollout-bucket))

;; RPC handlers run on serve threads where a parameterization from the
;; calling thread is invisible, so the tests' data-dir redirect is a box
;; (the same pattern the family uses), read at call time.
(define data-dir-override-box (box #f))

(define (set-update-data-dir-override! path)
  (set-box! data-dir-override-box path))

(define (update-data-dir)
  (or (unbox data-dir-override-box) (data-dir)))

(define (updater-state-path)
  (build-path (update-data-dir) "updater-state.json"))

(define (read-updater-state)
  (define path (updater-state-path))
  (if (file-exists? path)
      (with-handlers ([exn:fail? (lambda (_) (hasheq))])
        (define doc (with-input-from-file path read-json))
        (if (hash? doc) doc (hasheq)))
      (hasheq)))

(define (write-updater-state! doc)
  ;; Whitelist on write: unknown keys are dropped, so a corrupt or
  ;; hand-edited file can never grow product data into the updater store.
  (define clean
    (for/fold ([acc (hasheq)])
              ([key (in-list updater-state-keys)])
      (define value (hash-ref doc key #f))
      (if (exact-integer? value)
          (hash-set acc key value)
          acc)))
  (define path (updater-state-path))
  (define dir (path-only path))
  (when dir (make-directory* dir))
  (with-output-to-file path
    #:exists 'truncate/replace
    (lambda () (write-json clean))))

;; ---------- rollout bucket ----------

;; Stable random 0..99 assigned on first check so staged rollouts are
;; sticky per installation. Persisted as an integer in updater-state.json;
;; crypto-random-bytes, not `random`: racket/base's PRNG has a fixed seed,
;; which would put every fresh install in the same lockstep bucket.
(define (rollout-bucket)
  (define existing (hash-ref (read-updater-state) 'rollout-bucket #f))
  (if (and existing (exact-integer? existing) (<= 0 existing 99))
      existing
      (let ([bucket
             (modulo (integer-bytes->integer (crypto-random-bytes 4) #t #t)
                     100)])
        (write-updater-state!
         (hash-set (read-updater-state) 'rollout-bucket bucket))
        bucket)))

;; ---------- check ----------

;; Returns a plain hasheq describing the outcome; the backend maps it onto
;; the typed UpdateCheck record. Throttling is the host's job
;; (docs/UPDATE.md). fetch-update-manifest accepts only #:key-id and
;; #:maximum-bytes — redirection following (GitHub release assets answer
;; with a 302 into an otherwise empty body, rivet#153) is built in.
(define (perform-check!)
  (reset-update-state!)
  (state-set! 'phase "checking")
  (with-handlers
      ([exn:fail?
        (lambda (e)
          (state-set! 'phase "error")
          (state-set! 'message (exn-message e))
          (hasheq 'status "error" 'message (exn-message e)))])
    (define manifest
      (fetch-update-manifest (manifest-url)
                             (embedded-public-key)
                             #:key-id update-key-id))
    (define config
      (updater-config app-identifier
                      app-version
                      app-channel
                      (platform-symbol)
                      (architecture-symbol)
                      (embedded-public-key)
                      update-key-id
                      (rollout-bucket)
                      maximum-download-bytes))
    (define candidate (select-update config manifest))
    (cond
      [candidate
       (set-box! candidate-box candidate)
       (define artifact (update-candidate-artifact candidate))
       (state-set! 'phase "idle")
       (state-set! 'availableVersion
                   (update-manifest-version
                    (update-candidate-manifest candidate)))
       (hasheq 'status "available"
               'currentVersion app-version
               'availableVersion
               (update-manifest-version
                (update-candidate-manifest candidate))
               'build (update-manifest-build
                       (update-candidate-manifest candidate))
               'publishedAt
               (update-manifest-published-at
                (update-candidate-manifest candidate))
               'installer (symbol->string (update-artifact-installer artifact))
               'sizeBytes (update-artifact-size artifact))]
      [else
       (state-set! 'phase "idle")
       (state-set! 'availableVersion #f)
       (hasheq 'status "up-to-date" 'currentVersion app-version)])))

;; ---------- download ----------

;; The download lands in <data-dir>/updates/ under a self-describing name;
;; hosts learn the exact path from update-state's downloadedPath and never
;; parse the name themselves.
(define (destination-path data-dir candidate [extension (installer-extension)])
  (define version
    (update-manifest-version (update-candidate-manifest candidate)))
  (build-path data-dir
              "updates"
              (string-append app-display-name "-" version extension)))

(define (download-with-progress! config candidate destination)
  ;; rivet's downloader bounds the stream to the signed size, verifies
  ;; size + SHA-256 against the signed manifest before the file is
  ;; trusted, and swaps the .partial into place atomically. Progress
  ;; reports arrive as byte counts; publish integer percent to the state
  ;; box and feed the watchdog's stall clock on every report.
  (define total (update-artifact-size (update-candidate-artifact candidate)))
  (download-update config candidate destination
                   #:on-progress
                   (lambda (done)
                     (set-box! last-progress-seconds (current-seconds))
                     (state-set! 'percent
                                 (if (> total 0)
                                     (min 100 (quotient (* done 100) total))
                                     0)))))

;; Runs on a backend worker thread; the host follows progress via the
;; update-state RPC. Never raises: failures surface through the phase.
(define (start-download! data-dir)
  (define worker (unbox worker-thread-box))
  (when (and worker (thread-running? worker))
    (error 'start-download! "an update download is already running"))
  (define candidate (unbox candidate-box))
  (unless candidate
    (error 'start-download! "no update is available; run a check first"))
  (state-set! 'phase "downloading")
  (state-set! 'percent 0)
  (state-set! 'message #f)
  (define config
    (updater-config app-identifier
                    app-version
                    app-channel
                    (platform-symbol)
                    (architecture-symbol)
                    (embedded-public-key)
                    update-key-id
                    (rollout-bucket)
                    maximum-download-bytes))
  (define destination (destination-path data-dir candidate))
  (set-box! last-progress-seconds (current-seconds))
  (define worker-thread
    (thread
     (lambda ()
       ;; The watchdog breaks a stalled worker; exn:break is not an
       ;; exn:fail, so it needs its own arm — either way rivet's partial
       ;; cleanup removes the .partial and the phase lands on error
       ;; (never mid-flight).
       (with-handlers
           ([(lambda (e) (or (exn:fail? e) (exn:break? e)))
             (lambda (e)
               (state-set! 'phase "error")
               (state-set! 'message
                           (if (exn:break? e)
                               "update download stalled or timed out"
                               (exn-message e))))])
         (define path
           (download-with-progress! config candidate destination))
         (state-set! 'phase "downloaded")
         (state-set! 'percent 100)
         (state-set! 'downloadedPath (path->string path))))))
  (set-box! worker-thread-box worker-thread)
  ;; Watchdog: exits once the worker finishes on its own; otherwise a stall
  ;; (no byte progress) or the total wall-clock limit breaks the worker so
  ;; phase never rests on "downloading" indefinitely.
  (define download-started (current-seconds))
  (thread
   (lambda ()
     (let loop ()
       (cond
         [(sync/timeout download-stall-limit-seconds
                        (thread-dead-evt worker-thread))
          (void)]
         [(> (- (current-seconds) (unbox last-progress-seconds))
             download-stall-limit-seconds)
          (break-thread worker-thread)]
         [(> (- (current-seconds) download-started)
             download-total-limit-seconds)
          (break-thread worker-thread)]
         [else (loop)]))))
  (void))
