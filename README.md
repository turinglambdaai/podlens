# PodLens

Podcasts, in your language. A cross-platform desktop podcast player that transcribes, translates and summarizes English episodes with your own LLM API key — for listeners who understand far more reading than hearing.

[![CI](https://github.com/turinglambdaai/podlens/actions/workflows/ci.yml/badge.svg)](https://github.com/turinglambdaai/podlens/actions/workflows/ci.yml) ![macOS](https://img.shields.io/badge/macOS-SwiftUI-000000?logo=apple&logoColor=white) ![Windows](https://img.shields.io/badge/Windows-WinUI_3-0078D4?logo=windows11&logoColor=white) [![License](https://img.shields.io/badge/license-AGPL--3.0-blue)](LICENSE)

**English** · [中文](README.zh-CN.md)


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

## What ships in 1.3

- **One-click Understand**: download → transcribe → translate → summarize as a single job — finished stages are skipped on re-run, and a cost estimate (sentences/characters) shows before you start
- Subscribe to RSS podcast feeds (RSS 2.0 + iTunes tags), refresh with new-episode detection, auto-refresh every 30 minutes with new-episode notifications (macOS)
- Playback: 1.0–3.0× speed, ±15/30 s skip, sleep timer, chapter jumps (Podcasting 2.0 `podcast:chapters`), continuous playback into the next episode, resume-on-launch, media keys + Control Center (macOS) / SMTC (Windows)
- Sentence-level transcript with follow-along highlighting (tap a line to seek); sentence-aligned translation in bilingual / translation-only / original-only views
- Structured summaries (TL;DR, key points, quotes, topics) plus show notes from the feed
- Library basics: mark played/unplayed, unplayed badges per show, per-episode job progress
- Discover panel: curated catalog of classic English shows plus full-directory search (iTunes Search API, no key needed, results carry Apple's authoritative feed URLs) — nothing is ever auto-subscribed
- Bring-your-own-key: OpenAI, DeepSeek, Groq, SiliconFlow, Ollama, any OpenAI-compatible endpoint
- Agent-friendly CLI over the same core (`add`, `episodes`, `pipeline`,
  `estimate`, `transcribe`, `translate`, `summarize`, `show`, `export`,
  `find`, `done`, `search`, `--json`, exit codes 0/1/2)
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
raco pkg install --auto --no-docs --link /path/to/rivet
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
├── tests/                  # 19 backend tests incl. a fake OpenAI server
├── scripts/                # update-keys.sh, make-update-manifest.sh, make-icons.py
├── docs/                   # UPDATE.md (update contract) · DESIGN-ROADMAP.md (M1–M5)
├── site/                   # podlens.jrtx.site (GitHub Pages)
└── .github/workflows/      # ci.yml · release.yml · pages.yml
```

## Development

Prerequisites: [Racket CS](https://racket-lang.org/) (stable) with the Rivet package linked, and the platform toolchain (Xcode CLT on macOS, VS 2022 with the WinUI workload on Windows).

```bash
raco pkg install --auto --no-docs --link /path/to/rivet
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

## License

Licensed under the [AGPL-3.0 License](LICENSE).
