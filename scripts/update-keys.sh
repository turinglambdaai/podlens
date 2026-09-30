#!/usr/bin/env bash
# Generate the Ed25519 update-signing key pair for PodLens.
# The private key NEVER leaves this machine (and never enters the repo);
# the base64 raw public key gets embedded in the native hosts
# (UpdateService.swift / I18n+Update wiring) and handed to CI via secrets.
#
# Usage: scripts/update-keys.sh [private-key-path]
#   default private key path: ~/.podlens/update-signing-key.pem
#
# Requires OpenSSL 3 (Ed25519 needs `-rawin`; LibreSSL cannot do this).

set -euo pipefail

KEY_PATH="${1:-$HOME/.podlens/update-signing-key.pem}"

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

# ---- refuse to overwrite -----------------------------------------------------
if [ -e "$KEY_PATH" ]; then
  echo "error: $KEY_PATH already exists." >&2
  echo "Rotating keys needs a release that trusts the NEXT key first —" >&2
  echo "see docs/UPDATE.md. Move the old file away if you really mean it." >&2
  exit 1
fi

mkdir -p "$(dirname "$KEY_PATH")"
umask 077

"$OPENSSL_BIN_VALUE" genpkey -algorithm ed25519 -out "$KEY_PATH"
PUB_B64="$("$OPENSSL_BIN_VALUE" pkey -in "$KEY_PATH" -pubout -outform DER 2>/dev/null | tail -c 32 | base64)"

echo "private key: $KEY_PATH"
echo "public key (base64, embed in hosts): $PUB_B64"
