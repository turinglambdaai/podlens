# Changelog

All notable changes to PodLens are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versioning is SemVer.

## [Unreleased]

### Added
- **A Podcasts-grade shell on every platform.** All three hosts now
  share the same interaction model, Apple Podcasts style: a sidebar
  with Home and Discover destinations plus a Library section
  (Recently Updated, Shows, Downloaded) and cover-art subscription
  rows with unplayed badges; a Home page whose Up Next lane puts
  in-progress episodes first and then each show's newest unplayed
  episode as large artwork cards with a play pill and a menu
  (understand / download / mark played); a shows grid; a
  latest-episodes lane with see-all links; the player bar riding the
  window's bottom edge with cover art and a transcript shortcut; and
  episode rows everywhere showing cover art, Understand-pipeline
  status chips (transcript / translation / summary / downloaded /
  played) and mid-episode progress. Dates render as short "10月17日" /
  "Oct 17" instead of raw RFC-822. (macOS ships the full shell;
  Windows mirrors it on NavigationView; Linux mirrors it on a GTK4
  page stack with FlowBox tiles — Linux keeps its English-only UI,
  and Linux cover art streams through the system curl instead of a
  bundled HTTP stack, failing quietly to the placeholder.)
- **Cover art end to end.** `catalog-list` and `catalog-search` rows
  gain an `artwork-url` column: curated entries ship hand-verified
  Apple-CDN artwork, and iTunes search results upscale the 100px
  thumbnail to 600px on the same CDN path. Hosts load artwork through
  a memory-cached async loader over a disk-backed URL cache; feeds
  without a cover get a quiet placeholder instead of a letter tile.
- **`episode-recent` RPC** — the newest episodes across every
  subscription, one row = `(feed-id feed-title feed-artwork
  <episode-list row…>)`, so the home and library pages brand every
  card with show title and artwork in one round trip.
- Discover stops being a modal sheet (or, on Linux, a missing page):
  the sidebar's Discover entry opens a full page with directory
  search and the curated catalog as artwork cards. A first launch
  with an empty library lands there.

### Fixed
- Jobs no longer die inside their own error handler. The job-store
  update folded over `hash-set!` results, but `hash-set!` returns
  void, so the first multi-key patch — progress, done, or error —
  crashed with "hash-set!: contract violation given `#<void>`"; and
  since the error handler patched the same way, a failed download
  also left the job stuck "running", blocking every later job for
  that episode until restart. The updater now mutates the job hash in
  place and returns it, so progress lands, failures surface as job
  error messages, and double-click-to-listen downloads complete.
- Views observe the store directly now. The old views read the store
  through the app model, so store updates only landed when the model
  itself happened to re-render — lists drew once at boot and then
  froze.
- Boot RPCs are structured and sequential (feeds → catalog → resume →
  update check) and page-level loads no longer race the API binding;
  the per-step pipeline actions (download / transcribe / translate /
  summarize / delete audio / mark played) live in a More menu behind
  the primary Understand button.
- Update channel rebuilt against rivet HEAD: `updater-config` carries
  the new component field, the duplicate `platform-symbol` /
  `architecture-symbol` helpers are gone (rivet/distribution exports
  them now), and the local OS-to-extension helper is renamed to
  `payload-extension` so it no longer collides with rivet's
  installer-kind contract.

### Changed
- "Check for Updates…" moved next to About per the family menu
  convention: it now lives in the App menu on macOS, in a Help menu
  (Ctrl+U accelerator) in the Windows sidebar, and in the Help section
  of the Linux primary menu — replacing the Windows navigation item and
  the Linux Options entry.

## 0.2.0 - 2026-10-15

### Added
- **The Linux host ships.** A GTK4 first-party host lands in `linux/` —
  subscriptions, episode cards with status chips, feed add/remove/refresh
  (all + per-feed), resume-on-launch, one-click Understand with a cost
  estimate before the run, read-only transcript and summary panels, and
  GStreamer `playbin` playback over the local audio cache (±15/30 s skip,
  1.0–3.0× speed, chapter jumps, position writeback, click a transcript
  line to seek). Episode jobs (download/transcribe/translate/summarize)
  run with a job-progress bar; backend `notify` events surface as
  freedesktop notifications; the settings dialog and About block match the
  Windows host. The updater follows docs/UPDATE.md per install shape:
  AppImage swaps in place and offers a restart, deb/rpm installs point at
  the package manager, portable tarball installs get the extract hint —
  silent launch checks are throttled to one per 4 hours and skip dev
  copies and package-manager installs. UI text is English on Linux by
  design; the Windows and macOS hosts default to Chinese.
- **Linux release pipeline.** CI gains a `linux-host` job (GTK4 +
  GStreamer dev packages, embeddable Racket CS, `raco rivet build`,
  headless boot smoke under Xvfb); releases build x64 and arm64 artifacts
  — `tar.gz` (the signed update feed's Linux payload), `deb`, `rpm`,
  `AppImage` and portable `zip` — with stable versionless aliases
  (`podlens-linux-x64.deb`, `podlens-linux-x64.AppImage`, …). The deb
  payload ships the GStreamer plugin closure (base/good + `gst-libav`) so
  m4a/mp3 play on stock Debian/Ubuntu. The signed update manifest now
  carries the two Linux `tar.gz` entries next to macOS and Windows.
- **Every manual update check now reaches the screen.** The macOS sheet
  covers all terminal outcomes (the taskly UpdateSheet pattern): an
  available update offers install as before, an up-to-date answer gets a
  confirmation, and a failure shows the error with a Check-Again button.
- **The About surface lands on both hosts.** The macOS settings sheet
  and the Windows settings dialog close with an About block — version,
  license (AGPL-3.0), a Website link (podlens.jrtx.site) and a
  Report-an-Issue link (the GitHub issue tracker). A bug report now
  starts one click away from the failure it describes.
- **GitHub issue templates** (bug report + feature request) so feedback
  from the About link lands structured.

### Fixed
- **Understand runs the pipeline, not a stray summarize.** The Windows
  host's Understand button dispatched `"pipeline"` through a branch chain
  with no pipeline arm, so it silently fell through to
  `episode-summarize`. It now calls the real `episode-pipeline` RPC and
  refreshes transcript and summary when the job completes. (Linux keeps
  its event-driven job-progress bar; the Windows host still polls until
  the job leaves `running`.)
- **Switching episodes no longer resurrects the previous one.** Both
  hosts kept the last episode's media attached after stopping — Linux
  only reset the `playbin` state and Windows never cleared the player
  source — so the next Play/Pause toggled the old audio back to life
  under the new episode's UI. Episode switches now drop the media
  (`uri` / `Source`) along with the playback state.
- **Settings saves can no longer race shutdown.** The Linux host saved
  the settings dialog on a detached thread holding a synchronous
  `settings-set` call; the writes now ride the async API with each
  completion dispatched back to the main loop.
- **The seek bar respects an in-progress drag.** The Linux tick handler
  flipped the user-seeking flag every second, letting programmatic slider
  syncs fire seeks mid-drag; the sync now steps aside while the user is
  dragging.
- **Delete Audio reaches the Windows host.** The `action.remove_audio`
  string table entry had no button; the episode detail pane now shows
  Delete Audio while local audio exists, wired to the existing
  `episode-remove-audio` RPC (parity with Linux).

### Changed
- **Update failure text now leaves the screen with the user.** The macOS
  sheet's error is selectable, and the Windows update failure dialogs
  carry a "copy error" button that puts the failure message on the
  clipboard.
- CLI help text lists the `version` subcommand, which shipped earlier
  but was only discoverable through the dispatch table.

## 0.1.3 - 2026-10-14

### Changed

- **The updater is rebuilt on `rivet/distribution` and the shared
  `RivetUpdate` host module** (the family pattern, adopted by taskly
  first). The Racket engine retires its hand-rolled OpenSSL 3 subprocess
  scheme — manifest fetch/verify, update selection and the download now
  run through rivet's `fetch-update-manifest` / `select-update` /
  `download-update`, with the Ed25519 work done in-process (no PATH
  dependence, no temp-file dance). The macOS host shrinks to a product
  adapter over rivet 0.8.1's `RivetUpdate` module: delegate-driven
  download with request timeouts, signed-size enforcement, atomic swap
  with rollback, and a stale work-dir sweep. The download worker gained
  the family watchdog — no byte progress for 120 s or 30 minutes total
  breaks the thread, so a stalled network always reaches a terminal
  state instead of hanging the update. The sticky rollout bucket now
  draws from cryptographic randomness (the seeded PRNG would put every
  fresh install in the same lockstep bucket).
- CI and the macOS host consume a pinned rivet (`v0.11.0`): the
  workflows install rivet from its git tag instead of the moving catalog
  head, and `macos-host/Package.swift` consumes the root rivet package
  (`rivet#238`) instead of the retired `platform/macos` subpath.

### Added

- User manual: `site/guide.html` (bilingual, linked from the site nav) —
  five-minute start, subscriptions / transcription / translation /
  summary / BYOK setup, and a FAQ (data location, accounts, updates).
- The site shows a real macOS screenshot beside the Windows ones.
- Stable direct-download links (family standard): every release
  re-uploads its installers under version-less aliases
  (`podlens-macos-arm64.dmg`, `podlens-macos-x64.dmg`,
  `podlens-windows-x64.msi` plus the zips), the site's download cards
  and hero button point straight at them (JavaScript highlights the
  card for the detected platform, Intel Macs get the x64 DMG), and the
  Releases page is demoted to a secondary "all releases" link.

### Fixed

- The signed-manifest test suite selects its artifact by the running
  platform, so the backend tests pass on every OS again.
- Release pipeline: the Intel macOS runner's older swiftc (6.1.2)
  crashed in SILGen while emitting the update service's construction —
  the stored-property initializer, the lazy getter and a file-scope
  global constant all crashed (reproduced locally with a 6.1.2
  toolchain; newer toolchains compile the same code fine). The service
  is now a memoized computed property with the same lifetime, and its
  constructor is invoked with explicit arguments. The Windows
  collection step follows rivet v0.11.0's release layout, which emits
  the MSI and the portable zip directly in family form.

## 0.1.2 - 2026-10-12

### Security

- **Update-signing key rotation, phase 2 of 2**: the feed is now signed
  exclusively with the new Ed25519 key (fingerprint `b465070c92a4bb23`,
  embedded since 0.1.1); the previous key is retired and signs nothing
  anymore. Clients on 0.1.0 or older cannot verify this feed — update
  manually once (download the DMG/MSI); 0.1.1+ keeps updating in app.

### Changed

- Releases consume Rivet from the Racket Package Catalog instead of a
  checked-out working copy (the docs/ci.md standard) — first release
  through this flow.

## 0.1.1 - 2026-10-12

### Security

- **Update-signing key rotation, phase 1 of 2** (docs/UPDATE.md, Keys):
  this release embeds the *next* Ed25519 public key (fingerprint
  `b465070c92a4bb23`) in both hosts while the feed is still signed with the
  current key — 0.1.0 clients verify and auto-update as usual, and every
  client that lands here already trusts the key the next release is signed
  with. Phase 2 retires the current key and signs exclusively with the new
  one.

## 0.1.0 - 2026-10-10

> The family moves to the 0.x churn era (rivet itself is still 0.6.x), so
> this is a version reset from the 1.x line — 1.x clients see it as "up to
> date" (downgrades never nag) and update manually if they want to follow.

### Added

- **Windows host in-app updates** (the last host without them): the full
  family flow behind the existing "检查更新…" entry — silent throttled
  check at launch (at most once per 4 h, honoring `check-updates-enabled`),
  an available-update consent dialog, download with progress in the status
  line, and a "退出并安装" handoff script that waits for process exit,
  unpacks the portable zip, swaps the install directory (`.old` fallback),
  relaunches, and reports a failed install on the next launch. Portable
  (zip) installs update in place; MSI installs (Program Files) and dev
  copies keep the manual path — a dialog pointing at the releases page.
- Backend download channel: `update-check` now returns the typed
  `UpdateCheck` record (status/version/build/installer/size — no more
  localized strings on the wire), and new `start-download` / `update-state`
  RPCs run the artifact download on a backend worker thread with progress
  published to a state box. Downloads land under `~/.podlens/updates/`
  (never touching the data files) and are held to the signed manifest's
  size + SHA-256 before being trusted. The 4-hour throttle persists as the
  `last-update-check` config key, shared across hosts.
- `docs/UPDATE.md` status table: the Windows row is now "已换装".

### Fixed

- Downloads written through a text-mode port on Windows corrupt binary
  data (`\n` → `\r\n` expansion): `core/http.rkt`'s downloader now writes
  in binary mode — this affected episode audio too, and the updater's
  SHA-256 check would have rejected every archive. Wrapper verification's
  payload/signature scratch files get the same hardening.

## 1.4.0 - 2026-10-09

### Changed

- **Update feed converges to the family signed-wrapper format**: the two
  assets (`update-manifest.json` + `manifest.sig`) collapse into one
  self-contained signed wrapper under the same name — the inner manifest
  travels base64-encoded and the Ed25519 signature covers its exact bytes
  (signed by `rivet/distribution`'s `write-signed-manifest`). The macOS
  host parses the wrapper directly (key-id check included); the CLI,
  backend RPC and Windows report path verify through the same scheme.
  **Users of 1.3.2 and older update manually once** — their updaters read
  the old two-file format and now report a missing manifest (honest
  failure, never a false "up to date"); from 1.4.0 on the feed is
  self-contained again.
- Release artifacts renamed to the family scheme, all lowercase with the
  architecture spelled out: `podlens-<version>-macos-{arm64,x64}.dmg/.zip`
  (was `PodLens-v<version>-macos.dmg/.zip`), `podlens-<version>-windows-x64.zip`
  portable (was `PodLens-v<version>-windows-x64.zip`); per-artifact
  `.sha256` files ship beside them. The unused rivet-native
  `update-stable-windows.json` is no longer uploaded.

### Added

- **macOS Intel build**: the release pipeline builds both architectures
  (`macos-15-intel` for x64), and the macOS updater picks its feed entry
  by compile-time architecture, so Intel Macs get a native install path.
- Release version gate: a root `VERSION` file plus
  `scripts/check-release-version.sh` enforcing
  VERSION == rivet.rktd == app/version.rkt (and optionally == tag) in CI
  and before every release job.

### Fixed

- Backend boot no longer violates Swift 6 sendability: the startup ran
  `Task.detached { [backend, weak self] in … MainActor.run { self?.… } }`,
  which the older Xcode on the Intel release runner rejects ("sending
  'self' risks causing data races"). The boot now follows the family
  pattern: `backend.start()` is called synchronously (it only wires pipes
  and spawns the Racket thread — the runtime still boots off-main) and a
  plain `Task` inherits the main-actor isolation for boot completion.

## 1.3.2

### Security

- **Updater key rotated** (the previous private key had no vault backup —
  it lived only in CI — so the rotation also brings it into the keys
  vault). This build embeds the new public key; the published manifest
  and signatures are produced by the new key. Installs of 1.3.1 and
  older pin the retired key and must update manually.

### Fixed

- The artifact download now follows HTTP redirects (up to 5) — release
  assets answer with a 30x to their CDN, so the download leg of the
  in-app updater failed after a successful check. The manifest check
  already chased redirects.
- `raw-key->pem` passes an explicit empty line-terminator to
  `base64-encode`: this Racket build appends its second argument
  verbatim and the default value is garbage, which corrupted the
  embedded-key PEM round-trip.

## 1.3.1

### Fixed

- The update check follows HTTP redirects (rivet#153): GitHub release
  assets answer with a 302 to their CDN, and the previous fetch verified
  an empty redirect body — every in-app update check failed at signature
  verification. No app changes; rebuilt on the fixed rivet.


### Fixed

- The CLI/backend update check never verified a signature successfully:
  `raw-key->pem` appended a stray `#f` to the base64 body (this Racket
  build's `base64-encode` concatenates its second argument verbatim), and
  the signature file was written with `write-to-file`, which embeds the
  byte-string reader representation instead of raw bytes. Both paths write
  raw bytes now; a live check against the v1.3.0 release verifies the
  Ed25519 signature and reports correctly. The macOS host was unaffected
  (it verifies in-process via Swift); the Windows host and the CLI go
  through this code.

## 1.3.0 - 2026-10-08

### Added

- One-click "understand this episode": a `episode-pipeline` RPC (and
  `podlens pipeline <id>` on the CLI) chains download → transcribe →
  translate → summarize as a single job with one progress bar. Finished
  stages are skipped, so re-running after a failure only pays for what is
  missing. A prominent "一键听懂" button leads the episode header on both
  hosts.
- Cost visibility before committing (BYOK bills are real money): a new
  `episode-estimate` RPC returns duration/sentence/character counts and
  which stages are already done; the macOS header and Windows detail show
  the estimate under the action row, and `podlens estimate <id>` prints it.
- Show notes: the feed's episode description rides along in the
  `episode-list` row (11th column) and renders under the detail header on
  both hosts.
- Resume on launch: the backend records whichever episode last saved a real
  playback position (`resume-last` RPC); both hosts reopen that feed +
  episode at startup, seek to the recorded position and leave playback
  paused.
- Continuous playback: when an episode plays to its end, both hosts roll
  into the next (older) episode of the same feed — Apple-Podcasts style,
  without pretending there is an Up Next queue.
- Mark played / unplayed: `episode-set-done` RPC, a header toggle button on
  both hosts, and a right-click context menu on episode rows (Windows) /
  context menu (macOS). Unplayed resets the position.
- Unplayed counts: `feed-list` rows carry the per-feed unplayed count
  (8th column); the sidebar shows a badge next to the episode total.
- Chapter marks (Podcasting 2.0): feeds pointing at a
  `<podcast:chapters url=…>` JSON get their marks fetched and cached; new
  `episode-chapters` RPC; both player bars grow prev/next chapter buttons
  (hidden when the episode has no chapters).
- Auto refresh: both hosts refresh every subscription every 30 minutes.
  macOS posts a system notification when new episodes arrive; Windows
  surfaces the count in the status line (toasts need package identity —
  in-app notice only, honestly).
- CLI: `pipeline`, `estimate`, `done <id> [0|1]`, `export <id> [file]`
  (timestamped Markdown with translation, ready for notes) and
  `find <terms>…` (full-text search across every subscription's
  transcripts with sentence timestamps — "what did they say about X, at
  which minute" in one command).
- System media integration on both hosts: macOS routes hardware media keys
  and the Control Center now-playing widget through `MPRemoteCommandCenter`
  (play/pause, ±15/30 s skip, absolute seek, playback rate) with episode
  title, show name and show artwork in `MPNowPlayingInfoCenter`; Windows
  wires the System Media Transport Controls manually (taskbar flyout,
  hardware keys, Bluetooth headset buttons) with the same metadata and a
  1 Hz timeline feed. Next/previous are disabled on both — there is no
  queue yet.
- Player bar upgrades on both hosts: ±15/30 s skip buttons around play,
  sleep timer (5–90 min, pauses playback when it fires), playback speed
  extended to 3.0×, and keyboard control (Space = play/pause; ⌘← / ⌘→ on
  macOS, Ctrl+← / Ctrl+→ on Windows — inert while a text field owns focus).
- macOS: the transcript follow-along is driven by a 0.5 s time observer and
  only scrolls when the active sentence changes (previously it re-scrolled
  every 5 s regardless); position saves are throttled to ~6 s and pushed to
  Control Center alongside the elapsed time.
- Discovery search: the Discover panel (and `podlens search <terms>`) now
  search the full podcast directory through the iTunes Search API (free,
  no key; results carry Apple's authoritative feedUrl). Results reuse the
  catalog row shape, mark already-subscribed shows, and are added through
  the normal subscribe path. First launch with an empty library now opens
  on Discover instead of a blank window (macOS).
- CLI: `search <terms>` with `--json` output; `catalog` finally appears in
  `help` (it shipped in 1.1.0 but was never listed).
- `search-limit` setting (max results per search, default 25).

### Changed

- Regenerated typed clients against rivet main: the generated backend now
  embeds the deployment identity (name, version, channel) — rivet#97 is
  closed upstream, and `app/version.rkt` can retire onto it later.
- Windows window title now carries the product tagline
  ("PodLens — 听得懂的英文播客") instead of "PodLens — 播客工作台" —
  the product is a reading-level player, not a workbench (roadmap §1).
- Hosts honor `PODLENS_DATA_DIR` when locating cached audio (macOS +
  Windows, dev/test only); macOS restores the chosen playback rate after
  pause/resume; finished jobs are pruned when episode lists change.

### Fixed

- CLI ids are usable as printed: `list`/`episodes`/`find` show 12-char id
  prefixes, but every lookup required the full 40-char id, so the id a user
  copied from the output never worked. All id-taking commands (`episodes`,
  `refresh`, `download`, `pipeline`, `estimate`, `transcribe`, `translate`,
  `summarize`, `show`, `export`, `done`, `position`) now accept the full id
  or an unambiguous prefix; an ambiguous prefix fails with a distinct
  "ID 前缀不唯一 / Ambiguous id prefix" message.
- `podlens done <id> 1` stored the raw `member` result (a list) as the
  episode's `done` field instead of `#t` — `--json` answered
  `"done":["1","true","yes"]` and library.json carried a list where the
  hosts expect a boolean.
- New `tests/cli-test.rkt` covers the resolvers (exact / prefix /
  ambiguous / missing) and the real dispatch over a subprocess.

- Windows playback never worked (including 1.2.0): the `MediaPlayer` object
  was declared but never constructed, so every call in `StartPlayback`
  threw on the null projected object and surfaced as "playback failed".
  The player is created at startup now.
- macOS: the rate picker rendered 1.25× as "1.2×" (`%.2g` rounding); the
  labels now use `%g` and read 1×, 1.5×, 2.5×, 3×.
- Update checks no longer rely on a drifted hardcoded version: the backend
  and CLI share one `app/version.rkt` mirroring `rivet.rktd` (the
  deployment identity is not yet exposed by Rivet — turinglambdaai/rivet#97).
  Released 1.2.0 builds kept reporting "update available" against
  themselves through the backend RPC and CLI paths.
- RFC 822 feed dates with numeric UTC offsets were read as wall-clock UTC:
  the offset branch's regexp used `#rx` with `{4}`, which never fires, so
  every non-zero offset was silently ignored — and the feed test asserted
  the wrong value. `-0700`/`+0900` now shift correctly.
- Translation progress is persisted after every batch; a failed batch late
  in a long episode no longer discards the batches already paid for.
- Concurrent jobs on the same episode are rejected instead of racing the
  episode row and transcript file.
- `check-updates` reports failures with a localized message instead of
  swallowing the exception; update status strings are localized (zh) on
  the CLI and backend RPC paths.
- Silent ASR chunks (music, silence) advance the transcript timeline by
  the chunk window instead of collapsing subsequent timestamps; the
  multipart upload path closes its connection; the vendored pure-Racket
  SHA-256 was removed (digest utilities proposed upstream as
  turinglambdaai/rivet#99; the port-probing test workaround is
  turinglambdaai/rivet#98).

## 1.2.0 - 2026-09-30

### Added

- Windows MSI installer built by `raco rivet release` (WiX, per-machine,
  start-menu and desktop shortcuts, MajorUpgrade in-place upgrades), beside
  the existing portable zip. The Windows release job also emits the
  Rivet-native `update-stable-windows.json` manifest.
- Application icons on both platforms: `scripts/make-icons.py` draws the
  master and generates `assets/branding/app.ico` + `app.icns`, wired into
  `rivet.rktd` so the Windows exe (and MSI shortcuts) and the macOS bundle
  carry real artwork.

### Changed

- Windows host redesigned (roadmap M1, PR #3): three-pane `NavigationView`
  skeleton, episode rows with status chips, tabbed detail (transcript |
  summary), empty states, and a bottom player bar with seek and 1.0–2.0×
  speed — closing the gap with the macOS host. Both platforms now consume
  one design-token sheet (`windows/Themes/Tokens.xaml`,
  `macos-host/.../DesignTokens.swift`).

### Fixed

- Site download copy described the macOS DMG as universal; CI builds
  Apple Silicon only.

## 1.1.0 - 2026-09-30

### Added

- Curated podcast catalog (16 classic English shows across tech, security,
  science, design, business, news) behind a Discover panel — the backend
  exposes `catalog-list` with an `added` flag; hosts add entries through the
  normal subscribe path. Nothing is ever auto-subscribed.
- CLI: `catalog [category]` lists the catalog with subscribed marks
- `scripts/verify-catalog.rkt` re-verifies every entry against the live
  wire through the app's own HTTP/RSS stack (CI runs it soft-fail)

### Fixed

- Large feeds were truncated at the advertised Content-Length: NPR serves
  bodies larger than the header claims. Bodies are now read to EOF with
  `Connection: close`.
- Bare ampersands in real-world feeds (NPR: "Barnes & Noble") crashed the
  strict XML lexer; they are repaired before parsing.

## 1.0.2 - 2026-09-30

### Fixed

- macOS release builds were x86_64-only: setup-racket defaults to the x64
  Racket VM, which dragged the whole embedded host off-architecture. CI now
  installs the arm64 Racket CS on macOS runners; arm64 Macs no longer need
  Rosetta.

## 1.0.1 - 2026-09-30

### Fixed

- The Racket-side update client shipped without the embedded update public
  key, so the CLI's `check-updates` and the backend's update-check RPC
  always reported "developer build"; the key is now embedded (the SwiftUI
  host already carried it) and `check-updates` performs the full signed
  verification against GitHub Releases.

## 1.0.0 - 2026-09-30

First release. One Racket core, two first-party native hosts.

### Added

- RSS 2.0 + iTunes podcast feed subscriptions with refresh and new-episode detection
- Episode audio cache with resume-from-position playback and 1.0–2.0× speed (macOS)
- Per-episode AI pipeline: speech-to-text (any OpenAI-compatible ASR endpoint),
  sentence-aligned translation, structured summary (TL;DR, key points, quotes, topics)
- Transcript views: bilingual / translation-only / original-only, tap-to-seek follow-along
- Bring-your-own-key settings (api-base, api-key, models, target language) stored locally
- Agent-friendly CLI over the same core: add/list/episodes/refresh/download/transcribe/
  translate/summarize/show/position/config/check-updates/doctor, `--json`, exit codes 0/1/2
- Signed update channel: Ed25519-signed manifest + SHA-256 over GitHub Releases assets
- SwiftUI (macOS 14+) and WinUI 3 (Windows 10+) hosts over one RVT1 typed contract
- 18 backend tests including a local fake OpenAI server (no real key needed)
