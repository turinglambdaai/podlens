# Changelog

All notable changes to PodLens are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versioning is SemVer.

## Unreleased

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
