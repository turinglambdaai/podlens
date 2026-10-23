# PodLens user guide

**English** · [简体中文](USER-GUIDE.zh-CN.md)

Listen to podcasts and optionally create sentence-aligned transcripts, translations and summaries with your own model key.

## Availability

Free BYOK application. There is no PodLens hosted-AI subscription; your configured providers charge for their usage.

## Platforms and installation

Desktop previews for macOS, Windows and Linux. Mobile/companion applications are not delivered; Linux language selection currently uses PODLENS_LANG.

Use [official Releases](https://github.com/turinglambdaai/podlens/releases), select an explicitly listed OS/architecture asset, read its release notes and check supplied checksums. An absent asset/platform is not a delivery promise. Preview builds may lack publisher signing or notarization; do not disable system-wide security protections to install them.

## Your first successful session

1. Search Discover or add a podcast RSS URL, then play an episode without starting AI jobs.
2. Choose providers appropriate for audio transcription and text translation; they may require different model capabilities.
3. Test a short episode, inspect usage, then check transcript export and playback recovery before processing a large library.

## Data, backup and recovery

Data normally lives in ~/.podlens: config.json, library.json, audio/ and transcripts/. Quit before backup. config.json currently contains the API key in local storage; secure-storage migration is pending. Keep it out of shared backups, or encrypt the backup and rotate an exposed key.

## Network, keys and privacy

RSS, directory search and audio downloads contact their respective hosts. Transcription uploads selected audio to the chosen provider; translation/summary requests send text. Local Ollama availability does not imply local audio-transcription support.

## Known limits

Cancellation/recovery, secure storage and three-host packaged evidence remain tracked in #13. Windows MSI updates use a new installer rather than silently replacing the installation.

## Updates and removal

Back up data and read the destination release notes first. An in-app update check does not necessarily install an update for your channel; follow the manual for portable, installer and source paths. Never bypass a failed signature check or replace the program with an untrusted artifact. On failure, preserve data and the error, then recover with a trusted release. Removing the program does not necessarily erase its separate data directory; verify your backup before deleting personal data.

## Get help

Report problems in [GitHub Issues](https://github.com/turinglambdaai/podlens/issues), in English or Chinese. Include the version, OS/architecture, installation channel, steps, expected result and actual result. Reproduce with synthetic input first; do not attach keys, credentials, databases, private source documents or complete logs. Public support has no guaranteed response time.

## Further reading

- [Update behavior](../docs/UPDATE.md)
- [Project overview and development](../README.md)
- [Current licence](../LICENSE)
