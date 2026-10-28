#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
output="$repo/.rivet/acceptance/linux"
executable="$(find "$repo/dist" -type f -name RivetHost -perm -u+x -print -quit)"
data="$output/data-$$"
mkdir -p "$output" "$data"
[[ -n "$executable" && -x "$executable" ]]
cat >"$data/config.json" <<'EOF'
{"ui-lang":"en","check-updates-enabled":"false","last-update-check":2000000000}
EOF

export PODLENS_DATA_DIR="$data"
export HOME="$output/home-$$"
export XDG_RUNTIME_DIR="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/podlens-acceptance-runtime-$$"
export DISPLAY=:99
export GDK_BACKEND=x11
export GTK_A11Y=atspi
export NO_AT_BRIDGE=0
export LANG=C.UTF-8
unset WAYLAND_DISPLAY
mkdir -p "$HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
racket "$repo/scripts/e2e/seed-listening-fixture.rkt"

Xvfb "$DISPLAY" -screen 0 1440x900x24 -nolisten tcp \
  >"$output/xvfb.log" 2>&1 &
xserver_pid=$!
app_pid=""
atspi_pid=""

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
  [[ -n "$app_pid" ]] && kill -0 "$app_pid" 2>/dev/null && stop_bounded "$app_pid" || true
  [[ -n "$atspi_pid" ]] && kill -0 "$atspi_pid" 2>/dev/null && kill -TERM "$atspi_pid" || true
  kill -TERM "$xserver_pid" 2>/dev/null || true
  wait "$app_pid" "$atspi_pid" "$xserver_pid" 2>/dev/null || true
  exit "$status"
}
trap cleanup EXIT

for _ in $(seq 1 100); do
  [[ -S /tmp/.X11-unix/X99 ]] && break
  kill -0 "$xserver_pid" 2>/dev/null
  sleep 0.05
done
[[ -S /tmp/.X11-unix/X99 ]]
launcher="$(command -v at-spi-bus-launcher || true)"
[[ -n "$launcher" ]] || launcher=/usr/libexec/at-spi-bus-launcher
"$launcher" --launch-immediately >"$output/at-spi-bus.log" 2>&1 &
atspi_pid=$!
for _ in $(seq 1 100); do
  gdbus call --session --dest org.a11y.Bus --object-path /org/a11y/bus \
    --method org.a11y.Bus.GetAddress >/dev/null 2>&1 && break
  kill -0 "$atspi_pid" 2>/dev/null
  sleep 0.05
done
export AT_SPI_BUS_ADDRESS="$(gdbus call --session --dest org.a11y.Bus \
  --object-path /org/a11y/bus --method org.a11y.Bus.GetAddress |
  sed -E "s/^\('([^']+)',\)$/\1/")"

"$executable" >"$output/application.log" 2>&1 &
app_pid=$!
/usr/bin/python3 "$repo/scripts/e2e/linux-atspi.py" \
  --output "$output" --config "$data/config.json"

stop_bounded "$app_pid" || true
wait "$app_pid" 2>/dev/null || true
app_pid=""
trap - EXIT
kill -TERM "$atspi_pid" "$xserver_pid" 2>/dev/null || true
wait "$atspi_pid" "$xserver_pid" 2>/dev/null || true
