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

`rivet.rktd` is the single source of truth: bump `version` (SemVer) and
`build` (integer, never reuse). Add a matching `## X.Y.Z` section to
CHANGELOG.md — the tag validator rejects a tag without one.

## 2. Tagged pipeline

```bash
git tag -s v1.0.1 -m "PodLens 1.0.1"
git push origin v1.0.1
```

`.github/workflows/release.yml` then:

1. validates tag == rivet.rktd version == CHANGELOG section;
2. macOS: `raco rivet package` → DMG (human installer) + ditto zip (update feed);
3. Windows: `raco rivet package` → x64 zip;
4. release job: SHA256SUMS over all artifacts, `make-update-manifest.sh`
   signs the manifest with the CI secret, `gh release create --generate-notes`,
   Sigstore provenance attestation.

## 3. Publish checklist

- [ ] Release page lists DMG + zip + `update-manifest.json` + `manifest.sig` + `SHA256SUMS`
- [ ] `openssl pkeyutl -verify` passes with the PUBLIC key only
- [ ] Previous-version install reports "update available" on manual check
- [ ] Site download links match the release

## 4. Rollback

Never re-upload an artifact under an existing version URL — hashes are
pinned by the signed manifest. Pull = publish a new patch version whose
signed manifest points at it.
