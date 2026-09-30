#!/usr/bin/env bash
# Build and sign the PodLens update manifest (Taskly scheme).
#
# Usage: scripts/make-update-manifest.sh <tag> <dist-dir> [key-path]
#   <tag>       release tag, e.g. v1.0.0 (must match rivet.rktd version)
#   <dist-dir>  directory containing the packaged artifacts:
#                 PodLens-v<tag>-macos.zip
#                 PodLens-v<tag>-windows-x64.zip
#               (the DMG and the MSI are human installers, not manifest entries)
#   [key-path]  Ed25519 private key PEM; default $UPDATE_KEY_PATH, then
#               ~/.podlens/update-signing-key.pem
#
# Emits dist/update-manifest.json + dist/manifest.sig (raw 64-byte Ed25519
# signature over the exact manifest bytes) and dist/SHA256SUMS.
# Env overrides: RELEASE_ASSET_BASE_URL, OPENSSL_BIN.

set -euo pipefail

TAG="${1:?usage: make-update-manifest.sh <tag> <dist-dir> [key-path]}"
DIST="${2:?usage: make-update-manifest.sh <tag> <dist-dir> [key-path]}"
KEY_PATH="${3:-${UPDATE_KEY_PATH:-$HOME/.podlens/update-signing-key.pem}}"
VERSION="${TAG#v}"

# ---- locate an OpenSSL 3 -----------------------------------------------------
find_openssl() {
  local candidates=("${OPENSSL_BIN:-}" \
    /opt/homebrew/opt/openssl@3/bin/openssl \
    /usr/local/opt/openssl@3/bin/openssl \
    "$(command -v openssl || true)")
  for c in "${candidates[@]}"; do
    [ -n "$c" ] && [ -x "$c" ] || continue
    if "$c" version 2>/dev/null | grep -q "OpenSSL 3"; then
      echo "$c"; return 0
    fi
  done
  return 1
}

OPENSSL_BIN_VALUE="$(find_openssl)" || {
  echo "error: OpenSSL 3 not found; install with: brew install openssl@3" >&2
  exit 1
}

# ---- verify tag/version alignment -------------------------------------------
RKTD_VERSION="$(racket -e '(require racket/file) (displayln (hash-ref (file->value "rivet.rktd") (quote version)))' | tr -d '"')"
if [ "$VERSION" != "$RKTD_VERSION" ]; then
  echo "error: tag $VERSION != rivet.rktd version $RKTD_VERSION" >&2
  exit 1
fi

MACOS_ZIP="$DIST/PodLens-$TAG-macos.zip"
WIN_ZIP="$DIST/PodLens-$TAG-windows-x64.zip"

# ---- sha256 + size ------------------------------------------------------------
sha256_of() {
  shasum -a 256 "$1" | awk '{print $1}'
}
size_of() {
  if stat -f%z "$1" >/dev/null 2>&1; then stat -f%z "$1"; else stat -c%s "$1"; fi
}

BASE_URL="${RELEASE_ASSET_BASE_URL:-https://github.com/turinglambdaai/podlens/releases/download/$TAG}"

emit_platform() {
  local file="$1" key="$2"
  [ -f "$file" ] || { echo "error: missing $file" >&2; exit 1; }
  printf '    "%s": { "url": "%s/%s", "sha256": "%s", "size": %s }' \
    "$key" "$BASE_URL" "$(basename "$file")" "$(sha256_of "$file")" "$(size_of "$file")"
}

# ---- manifest ------------------------------------------------------------------
mkdir -p "$DIST"
MANIFEST="$DIST/update-manifest.json"
{
  printf '{\n'
  printf '  "version": "%s",\n' "$VERSION"
  printf '  "notesUrl": "%s",\n' "$BASE_URL"
  printf '  "platforms": {\n'
  FIRST=1
  if [ -f "$MACOS_ZIP" ]; then
    emit_platform "$MACOS_ZIP" "macos"; FIRST=0
  fi
  if [ -f "$WIN_ZIP" ]; then
    [ $FIRST -eq 1 ] || printf ',\n'
    emit_platform "$WIN_ZIP" "windows"; FIRST=0
  fi
  printf '\n  }\n}\n'
} > "$MANIFEST"

# ---- sign -----------------------------------------------------------------------
if [ ! -f "$KEY_PATH" ]; then
  echo "error: signing key not found at $KEY_PATH" >&2
  exit 1
fi
"$OPENSSL_BIN_VALUE" pkeyutl -sign -inkey "$KEY_PATH" -rawin \
  -in "$MANIFEST" -out "$DIST/manifest.sig"

# ---- checksums -------------------------------------------------------------------
find "$DIST" -maxdepth 1 -type f ! -name SHA256SUMS -print0 | sort -z \
  | xargs -0 shasum -a 256 > "$DIST/SHA256SUMS"

echo "manifest: $MANIFEST"
echo "signature: $DIST/manifest.sig"
echo "checksums: $DIST/SHA256SUMS"
