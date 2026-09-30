# Changelog

All notable changes to PodLens are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versioning is SemVer.

## Unreleased

### Changed

- Windows window title now carries the product tagline
  ("PodLens — 听得懂的英文播客") instead of "PodLens — 播客工作台" —
  the product is a reading-level player, not a workbench (roadmap §1).
- Hosts honor `PODLENS_DATA_DIR` when locating cached audio (macOS +
  Windows, dev/test only); macOS restores the chosen playback rate after
  pause/resume; finished jobs are pruned when episode lists change.

### Fixed

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
