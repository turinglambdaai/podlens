#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
output="$repo/.rivet/acceptance/macos"
app="$repo/dist/podlens.app/Contents/MacOS/podlens"
driver="$output/macos-ax"
home="$output/home-$$"
data="$output/data-$$"
mkdir -p "$output" "$home" "$data"
export PODLENS_DATA_DIR="$data"
[[ -x "$app" ]]
cat >"$data/config.json" <<'EOF'
{"ui-lang":"en","check-updates-enabled":"false","last-update-check":2000000000}
EOF
racket "$repo/scripts/e2e/seed-listening-fixture.rkt"

swiftc "$repo/scripts/e2e/macos-ax.swift" -o "$driver" \
  -framework AppKit -framework ApplicationServices

HOME="$home" "$app" >"$output/application.log" 2>&1 &
app_pid=$!

stop_bounded() {
  local pid="$1"
  kill -TERM "$pid" 2>/dev/null || true
  for _ in $(seq 1 200); do
    kill -0 "$pid" 2>/dev/null || return 0
    sleep 0.05
  done
  kill -KILL "$pid" 2>/dev/null || true
  return 1
}
cleanup() {
  local status=$?
  trap - EXIT
  if kill -0 "$app_pid" 2>/dev/null; then
    stop_bounded "$app_pid" || true
    wait "$app_pid" 2>/dev/null || true
  fi
  wait "$app_pid" 2>/dev/null || true
  # Preserve Apple's crash report alongside the accessibility snapshot. The
  # app runs with an isolated HOME, while CrashReporter may still use the
  # runner account's real Library depending on the host architecture.
  for diagnostics in \
    "$home/Library/Logs/DiagnosticReports" \
    "$HOME/Library/Logs/DiagnosticReports"; do
    if [[ -d "$diagnostics" ]]; then
      crash_report="$(find "$diagnostics" -type f \( -iname 'podlens*.ips' -o -iname 'podlens*.crash' \) -print -quit)"
      if [[ -n "$crash_report" ]]; then
        cp "$crash_report" "$output/application-crash.${crash_report##*.}"
      fi
    fi
  done
  exit "$status"
}
trap cleanup EXIT

"$driver" --pid "$app_pid" --output "$output" --config "$data/config.json"
for _ in $(seq 1 200); do
  kill -0 "$app_pid" 2>/dev/null || break
  sleep 0.05
done
if kill -0 "$app_pid" 2>/dev/null; then
  echo "PodLens did not exit after the native termination request" >&2
  exit 1
fi
wait "$app_pid"
trap - EXIT
