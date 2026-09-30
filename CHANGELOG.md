# Changelog

All notable changes to PodLens are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versioning is SemVer.

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
