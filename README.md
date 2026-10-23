# PodLens

Podcasts, in your language. A cross-platform desktop podcast player that transcribes, translates and summarizes English episodes with your own LLM API key — for listeners who understand far more reading than hearing.

[![CI](https://github.com/turinglambdaai/podlens/actions/workflows/ci.yml/badge.svg)](https://github.com/turinglambdaai/podlens/actions/workflows/ci.yml) [![Release](https://img.shields.io/github/v/release/turinglambdaai/podlens)](https://github.com/turinglambdaai/podlens/releases/latest) ![macOS](https://img.shields.io/badge/macOS-SwiftUI-000000?logo=apple&logoColor=white) ![Windows](https://img.shields.io/badge/Windows-WinUI_3-0078D4?logo=windows11&logoColor=white) [![License](https://img.shields.io/badge/license-AGPL--3.0-blue)](LICENSE)

**English** · [中文](README.zh-CN.md)

## Install

Download from [Releases](https://github.com/turinglambdaai/podlens/releases/latest):

| Platform | Download | Updates |
|---|---|---|
| macOS 14+ (Apple Silicon & Intel) | `podlens-<version>-macos-<arch>.dmg` — `arm64` / `x64` (ad-hoc signed — first launch: right-click → Open) | in-app (signed manifest), or reinstall the newer DMG |
| Windows 10+ x64 | `podlens-<version>-windows-x64.msi` installer (start-menu & desktop shortcuts), or `podlens-<version>-windows-x64.zip` portable | portable: in-app download & install (signed manifest); MSI: re-run the newer installer (the app points you to it) |
| Linux x64 / arm64 | `podlens-<version>-linux-x64.AppImage` / `podlens-<version>-linux-arm64.AppImage` (bundled GTK4), `podlens-<version>-linux-<distro-arch>.deb` (ships the GStreamer plugin closure incl. `gst-libav` for m4a/mp3), `.rpm`, or the portable `tar.gz`/`.zip` | tarball/AppImage: in-app check + download (signed manifest); deb/rpm: upgrade through the package manager |

Every release carries a `SHA256SUMS` manifest and Sigstore build provenance
(`gh attestation verify <file> -R turinglambdaai/podlens`).

The macOS build is ad-hoc signed — on first launch, right-click the app and
choose Open to clear the Gatekeeper prompt. Updates arrive in-app through an
Ed25519-signed manifest ([docs/UPDATE.md](docs/UPDATE.md)).

## Why PodLens?

Listening comprehension is the last wall for most non-native English speakers. Feeds have no translation, nobody summarizes an hour of talk, and pausing to look things up kills the flow. PodLens fixes this with a per-episode pipeline:

- **Transcribe** — speech-to-text through any OpenAI-compatible `/audio/transcriptions` endpoint (Whisper and friends), with timestamps per sentence
- **Translate** — sentence-aligned translation into Chinese (or English), rendered alongside the original
- **Summarize** — a TL;DR, 5–8 key points, notable quotes and topic tags for every episode

The transcripts, translations and summaries are cached on disk, so an episode is processed once and stays yours.

### How it compares

| | PodLens | Apple Podcasts | Overcast |
|---|---|---|---|
| Native UI | **SwiftUI / WinUI 3** | AppKit | web wrapper |
| Transcript | **on demand, yours** | limited podcasts | limited podcasts |
| Translation | **per sentence, local cache** | — | — |
| Summary | **TL;DR + key points** | AI recaps (US only) | — |
| LLM provider | **BYOK, any OpenAI-compatible** | Apple-only | — |

## How it works

PodLens is built on [Rivet](https://github.com/turinglambdaai/rivet): one shared Racket backend (feed parsing, library storage, the AI pipeline, the update client) embedded into each first-party native host. Not Electron, not a web wrapper.

```text
                 Racket application core
      feeds · library · ASR/translate/summarize · updater
                          │
                    RVT1 protocol
                 typed RPC / Events
                    ┌─────┴─────┐
                macOS         Windows
               SwiftUI        WinUI 3
```

- The backend embeds Racket CS in-process — no helper daemon, no console window
- Playback uses the platform media stack; the audio cache lives in `~/.podlens/audio/`
- The UI talks only to the typed contract in `app/backend.rkt` — changing it is a cross-platform release

## What's inside

- **One-click Understand**: download → transcribe → translate → summarize as a single job — finished stages are skipped on re-run, and a cost estimate (sentences/characters) shows before you start
- Subscribe to RSS podcast feeds (RSS 2.0 + iTunes tags), refresh with new-episode detection, auto-refresh every 30 minutes with new-episode notifications (macOS)
- Playback: 1.0–3.0× speed, ±15/30 s skip, sleep timer, chapter jumps (Podcasting 2.0 `podcast:chapters`), continuous playback into the next episode, resume-on-launch, media keys + Control Center (macOS) / SMTC (Windows) / MPRIS (Linux)
- Sentence-level transcript with follow-along highlighting (tap a line to seek); sentence-aligned translation in bilingual / translation-only / original-only views
- Structured summaries (TL;DR, key points, quotes, topics) plus show notes from the feed
- Library basics: mark played/unplayed, unplayed badges per show, per-episode job progress
- Discover panel: curated catalog of classic English shows plus full-directory search (iTunes Search API, no key needed, results carry Apple's authoritative feed URLs) — nothing is ever auto-subscribed
- Try before you subscribe: a one-click audition on every Discover card peeks the feed, caches the newest episode's audio and plays it outside your library (`feed-peek` / `preview-audio`; CLI: `peek`, `preview`)
- Bring-your-own-key: OpenAI, DeepSeek, Groq, SiliconFlow, Ollama, any OpenAI-compatible endpoint
- Agent-friendly CLI over the same core (`add`, `episodes`, `pipeline`,
  `estimate`, `transcribe`, `translate`, `summarize`, `show`, `export`,
  `find`, `done`, `search`, `peek`, `preview`, `--json`, exit codes 0/1/2)
- Signed in-app updates (Ed25519 manifest + SHA-256, see [docs/UPDATE.md](docs/UPDATE.md))

## Quick Start

### 1. Configure your API key

Open Settings and fill in:

- `api-base` — e.g. `https://api.openai.com/v1` (or DeepSeek/Groq/Ollama base)
- `api-key` — your key, stored locally in `~/.podlens/config.json`, never synced

### 2. Listen

Add a podcast RSS URL, pick an episode, press **转写 → 翻译 → 总结** (transcribe → translate → summarize). Or from a terminal:

```bash
git clone https://github.com/turinglambdaai/podlens && cd podlens
raco pkg install --auto --no-docs 'https://github.com/turinglambdaai/rivet.git#v0.11.0'
racket app/cli.rkt add "https://feeds.example.com/show.xml"
racket app/cli.rkt episodes <feed-id>
racket app/cli.rkt transcribe <episode-id>
racket app/cli.rkt show <episode-id> --json
```

The CLI is the same Racket source the GUI embeds, run headless. (Shipping a
single self-contained CLI binary is on the roadmap; inside the repo it needs
no API key for `--help`, `list`, `doctor` and `config`.)

## Repository layout

```text
podlens/
├── rivet.rktd              # release identity, version, deployment targets
├── app/
│   ├── backend.rkt         # the RVT1 wire contract (21 RPCs, 5 events, 1 state)
│   ├── update.rkt          # signed-manifest update checks
│   ├── cli.rkt             # agent-facing CLI (--json, exit codes)
│   └── core/               # feeds, library, config, openai, pipeline, chapters, i18n
├── macos-host/             # SwiftUI host (player, transcript, updater)
├── windows/                # WinUI 3 host (C++/WinRT code-behind)
├── linux/                  # GTK4 host (GStreamer playback; GeneratedBackend.hpp is generated)
├── tests/                  # 41 backend tests incl. a fake OpenAI server
├── scripts/                # update-keys.sh, make-update-manifest.sh, make-icons.py
├── docs/                   # UPDATE.md (update contract) · DESIGN-ROADMAP.md (M1–M5)
├── site/                   # podlens.jrtx.site (GitHub Pages)
└── .github/workflows/      # ci.yml · release.yml · pages.yml
```

## Development

Prerequisites: [Racket CS](https://racket-lang.org/) (stable) with the pinned Rivet package installed, and the platform toolchain (Xcode CLT on macOS, VS 2022 with the WinUI workload on Windows, GTK4 + GStreamer dev packages on Linux).

```bash
raco pkg install --auto --no-docs 'https://github.com/turinglambdaai/rivet.git#v0.11.0'
raco rivet doctor
raco rivet dev          # build + launch the current platform's host
raco test tests/        # backend tests (no API key needed — fake server)
```

## Honest gaps

- **No waveform, gapless playback or Up Next queue** — seek, ±15/30 s skip, 1.0–3.0× speed, sleep timer, chapter jumps and continuous playback are there; a drag-reorderable queue is not
- **Chapters only via Podcasting 2.0 feed pointers** — embedded ID3/M4A chapter atoms are not parsed; per-chapter summaries are deliberately not built (N× the LLM bill)
- **Translation cost is bounded only by your usage** — every sentence of a chosen episode goes through your API; long episodes cost real money. The header shows a sentence/character estimate before you start, and `podlens estimate <id>` prints it
- **One target language at a time** — the pipeline retranslates when you change `target-lang`
- **macOS builds are ad-hoc signed** until notarization credentials are configured in CI; first launch needs right-click → Open

## Roadmap

The full design roadmap (M1 consistency baseline → M2 transcript-as-product → M3 utility loop → M4 mobile companion → M5 back to Rivet) lives in [docs/DESIGN-ROADMAP.md](docs/DESIGN-ROADMAP.md).

- [x] RSS subscription + episode cache + playback positions
- [x] ASR / translation / summary pipeline with job progress events
- [x] One-click "understand this episode" (pipeline RPC) + cost estimates
- [x] SwiftUI + WinUI 3 hosts over one RVT1 contract
- [x] Ed25519-signed update channel
- [x] System media keys (Control Center / SMTC), skip, sleep timer, 3× speed
- [x] Chapter marks (Podcasting 2.0), mark played, unplayed badges, continuous playback, resume on launch
- [ ] Notarized macOS + Authenticode-signed Windows releases
- [ ] Per-chapter summaries, listening stats and vocabulary export
- [ ] Mobile companion — iOS / iPadOS + Android hosts, watchOS remote
      (design in [docs/DESIGN-ROADMAP.md](docs/DESIGN-ROADMAP.md))

## License

Licensed under the [AGPL-3.0 License](LICENSE).

## Feedback and diagnostics

Report reproducible bugs and feature requests through [GitHub Issues](https://github.com/turinglambdaai/podlens/issues). Include the release, OS/architecture and steps; inspect and redact diagnostics before sharing. Do not post API keys, license files, vault contents or private documents. For sensitive security reports, follow SECURITY.md when present; otherwise request a private contact route without disclosing sensitive details. Nothing is uploaded automatically.

## Product & pricing

Free BYOK listening and reading tool. Read the complete [English pricing and product policy](PRICING.md) or [简体中文](PRICING.zh-CN.md) for target prices, free/paid boundaries and launch gates. This is a launch design, not a live offer; this documentation does not add payment restrictions to current development builds.
