#!/usr/bin/env bash
# Flash the launcher onto a Waveshare ESP32-S3-Touch-AMOLED-1.8.
# Needs Python 3 and a USB-C cable. Does NOT need ESP-IDF.
set -euo pipefail

PORT="${1:-}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MIN_ESPTOOL="4.0"

if [ -z "$PORT" ]; then
  MATCHES=$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null || true)
  PORT=$(echo "$MATCHES" | head -1)
  COUNT=$(printf '%s\n' "$MATCHES" | grep -c . || true)
  if [ "$COUNT" -gt 1 ]; then
    OTHERS=$(printf '%s\n' "$MATCHES" | tail -n +2 | tr '\n' ' ')
    echo "Multiple boards found; using $PORT" >&2
    echo "Other ports seen, not used: $OTHERS" >&2
  fi
fi
if [ -z "$PORT" ]; then
  echo "No board found. Plug it in, or pass the port: ./flash.sh /dev/cu.usbmodem101" >&2
  exit 1
fi

# Reports "<version> ok" if an importable esptool is >= $MIN_ESPTOOL,
# "<version> old" if it's importable but too old, or fails (no output) if
# esptool isn't importable at all.
probe_esptool() {
  "$1" - "$MIN_ESPTOOL" <<'PYEOF'
import sys
try:
    import esptool
except ImportError:
    sys.exit(1)

def parse(v):
    parts = []
    for chunk in v.split("."):
        digits = ""
        for ch in chunk:
            if ch.isdigit():
                digits += ch
            else:
                break
        parts.append(int(digits) if digits else 0)
    return tuple(parts)

ver = getattr(esptool, "__version__", "0")
min_ver = sys.argv[1]
status = "ok" if parse(ver) >= parse(min_ver) else "old"
print(f"{ver} {status}")
PYEOF
}

venv_failure() {
  cat >&2 <<VENVFAIL

Could not install esptool automatically.

Likely causes: no network connection, a proxy blocking PyPI, or this
Python is missing the built-in "venv" module.

Manual fallback:
  1. python3 -m pip install --user esptool
  2. Run this script again.
VENVFAIL
  exit 1
}

SYS_VER=""
SYS_STATUS="missing"
if RESULT=$(probe_esptool python3 2>/dev/null); then
  SYS_VER="${RESULT% *}"
  SYS_STATUS="${RESULT##* }"
fi

if [ "$SYS_STATUS" = "ok" ]; then
  ESPTOOL="python3 -m esptool"
  echo "Using esptool $SYS_VER from $(command -v python3) (already installed)"
else
  if [ "$SYS_STATUS" = "old" ]; then
    echo "System esptool is $SYS_VER, need >= $MIN_ESPTOOL -- installing a newer one into a local venv." >&2
  fi
  echo "Installing esptool into a local venv..."
  python3 -m venv "$HERE/.venv" || venv_failure
  "$HERE/.venv/bin/pip" install --quiet "esptool>=$MIN_ESPTOOL" || venv_failure
  ESPTOOL="$HERE/.venv/bin/python3 -m esptool"
  VENV_VER=$("$HERE/.venv/bin/python3" -c "import esptool; print(esptool.__version__)")
  echo "Using esptool $VENV_VER from $HERE/.venv (freshly installed)"
fi

# The binaries are no longer committed to the repo -- they are a release
# asset. Inside an extracted release, bin/ is already here and this is a
# no-op. Run from a git checkout, it fetches the release instead.
RELEASE_REPO="${LAUNCHER_REPO:-rickymetz/esp32-hackathon}"
RELEASE_TAG="${LAUNCHER_VERSION:-latest}"

if [ ! -d "$HERE/bin" ]; then
  echo "No local bin/ -- fetching the $RELEASE_TAG release from $RELEASE_REPO ..."
  CACHE="$HERE/.cache"
  mkdir -p "$CACHE"

  if [ "$RELEASE_TAG" = "latest" ]; then
    API="https://api.github.com/repos/$RELEASE_REPO/releases/latest"
  else
    API="https://api.github.com/repos/$RELEASE_REPO/releases/tags/$RELEASE_TAG"
  fi

  ASSET=$(curl -fsSL "$API" 2>/dev/null | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    sys.exit(1)
for a in d.get('assets', []):
    if a['name'].endswith('.zip'):
        print(a['browser_download_url']); break
" || true)

  if [ -z "$ASSET" ]; then
    cat >&2 <<NOREL

Could not find a release asset.

Either no release has been published yet, or this machine cannot reach
GitHub. To flash from a local build instead:

  cd launcher && idf.py build
  tools/package_firmware.sh v0.0.0-local
  cd dist/esp32-launcher-v0.0.0-local && ./flash.sh

NOREL
    exit 1
  fi

  echo "Downloading $(basename "$ASSET") ..."
  curl -fSL --progress-bar -o "$CACHE/release.zip" "$ASSET"
  rm -rf "$CACHE/extract" && mkdir -p "$CACHE/extract"
  unzip -q "$CACHE/release.zip" -d "$CACHE/extract"
  # The archive holds a single top-level directory.
  SRC=$(find "$CACHE/extract" -maxdepth 2 -type d -name bin | head -1)
  [ -n "$SRC" ] || { echo "release archive has no bin/ directory" >&2; exit 1; }
  ln -sfn "$SRC" "$HERE/bin"
  [ -f "$(dirname "$SRC")/MANIFEST" ] && cp "$(dirname "$SRC")/MANIFEST" "$HERE/MANIFEST"
fi

if [ -f "$HERE/MANIFEST" ]; then
  # Parsed, never sourced: MANIFEST is data, and its values contain spaces
  # ("idf=ESP-IDF v5.5.5"). Sourcing it made the shell try to execute the
  # version string as a command.
  manifest_get() { sed -n "s/^$1=//p" "$HERE/MANIFEST" | head -1; }
  echo "Flashing $(manifest_get version) (commit $(manifest_get commit), built $(manifest_get built_utc))"
  if [ "$(manifest_get dirty)" = "yes" ]; then
    echo "  note: built from a dirty working tree" >&2
  fi
fi

# Offsets and write_flash flags come from the build (bin/OFFSETS,
# bin/FLASH_ARGS), not from this script. Hardcoding them is what let the
# previous version ship without srmodels.bin -- three of the four required
# images -- leaving voice silently non-functional.
if [ ! -f "$HERE/bin/OFFSETS" ]; then
  echo "bin/OFFSETS missing -- this bin/ predates the release format." >&2
  exit 1
fi

FLASH_ARGS=$(cat "$HERE/bin/FLASH_ARGS")
IMAGES=()
while read -r off file; do
  [ -n "$off" ] || continue
  [ -f "$HERE/bin/$file" ] || { echo "missing image: $file" >&2; exit 1; }
  IMAGES+=("$off" "$HERE/bin/$file")
done < "$HERE/bin/OFFSETS"

echo "Flashing $PORT ..."
# shellcheck disable=SC2086
if ! $ESPTOOL --chip esp32s3 --port "$PORT" -b 460800 \
    --before default_reset --after hard_reset write_flash \
    $FLASH_ARGS "${IMAGES[@]}"; then
  cat >&2 <<'RECOVERY'

Flashing failed.

If the board previously ran an app that crashed, its USB is wedged and no
software reset can recover it. Do this by hand:

  1. Hold PWR for at least 6 seconds to power off.
  2. Hold BOOT down and keep holding it.
  3. Press PWR to power on, then release BOOT.
  4. Run this script again.
  5. Afterwards power-cycle again -- it does not leave download mode on its own.

Also check nothing else is holding the port (a serial monitor will block it).
RECOVERY
  exit 1
fi

echo
echo "Done. Put your .lua apps in /apps on the microSD card."
