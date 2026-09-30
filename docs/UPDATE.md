# PodLens Update Contract

PodLens uses the Taskly scheme: the update feed travels as GitHub Release
assets, trust comes from an Ed25519 signature, never from HTTPS alone.

## Feed layout

Every release uploads two special assets alongside the installers:

- `update-manifest.json` — the signed payload
- `manifest.sig` — raw 64-byte Ed25519 signature over the **exact** manifest bytes

The client resolves `https://api.github.com/repos/turinglambdaai/podlens/releases/latest`,
finds both assets, downloads them, verifies the signature, and only then
parses the JSON.

## Manifest schema

```json
{
  "version": "1.0.1",
  "notesUrl": "https://github.com/turinglambdaai/podlens/releases/download/v1.0.1",
  "platforms": {
    "macos":   { "url": "https://github.com/…/PodLens-v1.0.1-macos.zip",
                 "sha256": "<lowercase hex>", "size": 12345678 },
    "windows": { "url": "https://github.com/…/PodLens-v1.0.1-windows-x64.zip",
                 "sha256": "<lowercase hex>", "size": 23456789 }
  }
}
```

- `version` — release version without the `v` prefix; must equal `rivet.rktd`'s `version`
- platforms is keyed `macos` / `windows`; each value is `{url, sha256, size}`
- asset naming: `PodLens-v<tag>-{macos.zip,windows-x64.zip}` (the update archives; DMG/other artifacts are for humans)

## Trust model, in verify order

1. Ed25519 signature over the manifest bytes (embedded public key; CryptoKit on macOS, OpenSSL 3 for the CLI)
2. Semver comparison — numeric per dot segment, missing segments are 0; downgrades report "up to date"
3. SHA-256 of the complete downloaded archive against the signed value
4. (macOS) the swapped-in bundle's `CFBundleShortVersionString` must equal `manifest.version` before the old bundle is replaced

Failed installs roll back: the macOS updater swaps atomically via a shell
script (`mv` old aside → `mv` new in → relaunch; on failure the old bundle
is restored). `~/.podlens/` is never touched by an update.

## Keys

- Generation: `scripts/update-keys.sh` → `~/.podlens/update-signing-key.pem`
  (OpenSSL 3 Ed25519; refuses to overwrite; prints the base64 raw public key)
- The **public key** is embedded in hosts: `UpdateService.swift` (`publicKeyBase64`)
  and `app/update.rkt` (`current-update-public-key-hex`, base64 of the raw 32 bytes)
- The **private key** lives outside any repository and reaches CI only as the
  `UPDATE_ED25519_PRIVATE_KEY` secret (PEM), consumed by
  `scripts/make-update-manifest.sh` in the release job
- Rotation: ship a release trusting the NEXT public key first, then sign
  exclusively with the new private key

## Client behavior

- Manual check: menu "检查更新…" / CLI `check-updates` — always reports the outcome
- Silent check (macOS): at most once per 4 h after launch; never nags; a
  found update shows a confirm dialog before installing
- Developer builds (no embedded key, or app not in /Applications) report
  honestly that updates are unavailable instead of pretending to check
