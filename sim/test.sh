#!/usr/bin/env bash
# Render-test every app through the simulator.
#
# For each app in apps/ AND each fixture in tests/fixtures/ (except the
# intentional ones below) this runs the
# app, lets it settle, and asserts the frame is non-blank -- i.e. the app loads
# and actually draws something. Exits non-zero if any app fails, so CI can gate
# on it. PNGs are written under sim/build/shots/ for inspection.
#
# Usage: sim/test.sh [--timeout N]
set -uo pipefail

cd "$(dirname "$0")/.."          # repo root
SIM=sim/build/sim
SHOTS=sim/build/shots
TIMEOUT=10

while [ $# -gt 0 ]; do
    case "$1" in
        --timeout) TIMEOUT="$2"; shift 2 ;;
        *) echo "unknown arg: $1"; exit 2 ;;
    esac
done

if [ ! -x "$SIM" ]; then
    echo "sim not built -- run sim/build.sh first" >&2
    exit 2
fi
mkdir -p "$SHOTS"

# Intentional fixtures that are NOT shippable apps:
#   *_error / broken / hook_bypass -- meant to fail on purpose
#   runaway_*                      -- infinite loops (watchdog fodder)
#   headless / trim_check          -- no UI by design (timers/print only)
SKIP=" broken cb_error deep_error hook_bypass runaway_bare runaway_coro runaway_pcall headless trim_check "

# Every app is either a flat apps/<name>.lua or a folder apps/<name>/main.lua.
# Fixtures are always flat, under tests/fixtures/.
# List both as "<name>=<path>" so the loop renders folder apps (the self-
# contained demos) exactly like flat ones.
apps=()
for app in apps/*.lua tests/fixtures/*.lua; do
    [ -e "$app" ] || continue
    apps+=("$(basename "$app" .lua)=$app")
done
for main in apps/*/main.lua; do
    [ -e "$main" ] || continue
    apps+=("$(basename "$(dirname "$main")")=$main")
done

pass=0; fail=0; skip=0; failed_apps=""
for entry in "${apps[@]}"; do
    name="${entry%%=*}"
    app="${entry#*=}"
    if [[ "$SKIP" == *" $name "* ]]; then
        printf "  SKIP  %s\n" "$name"; skip=$((skip+1)); continue
    fi
    out="$SHOTS/$name.png"
    if "$SIM" --sdroot . --timeout "$TIMEOUT" \
            run "$app" : sleep 0.4 : check "$out" >/dev/null 2>&1; then
        printf "  ok    %s\n" "$name"; pass=$((pass+1))
    else
        printf "  FAIL  %s\n" "$name"; fail=$((fail+1)); failed_apps="$failed_apps $name"
    fi
done

echo
echo "apps: $pass ok, $fail failed, $skip skipped   (shots in $SHOTS/)"
if [ "$fail" -ne 0 ]; then
    echo "failed:$failed_apps" >&2
    exit 1
fi
