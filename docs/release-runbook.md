# Release runbook

How a PodLens release ships. The operator document; `docs/UPDATE.md`
explains the trust model.

## 0. Prerequisites (one-time)

- Ed25519 update key pair: `scripts/update-keys.sh` (stored outside the repo)
- GitHub secret `UPDATE_ED25519_PRIVATE_KEY` = the PEM contents:
  ```bash
  gh secret set UPDATE_ED25519_PRIVATE_KEY < ~/.podlens/update-signing-key.pem -R turinglambdaai/podlens
  ```
- (later, for signed/notarized installers) Apple Developer ID + notarytool
  profile, Windows Authenticode certificate — release.yml picks these up
  automatically when the secrets exist

## 1. Version bump

The root `VERSION` file, `rivet.rktd` and `app/version.rkt` carry the same
version; bump all three (`scripts/check-release-version.sh` enforces the
alignment locally, in CI and in the release pipeline). Bump `rivet.rktd`'s
`build` (integer, never reuse). Add a matching `## X.Y.Z` section to
CHANGELOG.md — the tag validator rejects a tag without one.

## 2. Tagged pipeline

```bash
git tag -s v1.4.0 -m "PodLens 1.4.0"
git push origin v1.4.0
```

`.github/workflows/release.yml` then:

1. validates tag == VERSION == rivet.rktd == app/version.rkt == CHANGELOG section;
2. macOS (arm64 + x64 matrix): `raco rivet package` → DMG (human installer)
   + ditto zip (update feed) + per-artifact sha256;
3. Windows: `raco rivet release` → x64 MSI + portable zip + sha256;
4. release job: `make-update-manifest.sh` signs the single-file wrapper
   (`update-manifest.json`) with the CI secret, SHA256SUMS from the
   per-artifact sums, `gh release create --generate-notes`, Sigstore
   provenance attestation.

## 3. Publish checklist

- [ ] Release page lists per-arch DMG + zip (`podlens-<ver>-macos-{arm64,x64}.{dmg,zip}`),
      `podlens-<ver>-windows-x64.{msi,zip}`, per-artifact `.sha256`,
      the signed `update-manifest.json` wrapper and `SHA256SUMS`
- [ ] `openssl pkeyutl -verify` over the wrapper **payload** passes with the
      PUBLIC key only (extract the payload from the wrapper JSON first)
- [ ] Previous-version install reports "update available" on manual check
- [ ] Site download copy matches the release (platform/arch coverage)

## 4. Rollback

Never re-upload an artifact under an existing version URL — hashes are
pinned by the signed manifest. Pull = publish a new patch version whose
signed manifest points at it.
