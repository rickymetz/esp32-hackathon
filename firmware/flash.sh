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

if [ -f "$HERE/bin/BUILD_INFO" ]; then
  # shellcheck disable=SC1090
  . "$HERE/bin/BUILD_INFO"
  echo "Flashing build ${commit:-unknown} from ${date:-unknown}"
fi

echo "Flashing $PORT ..."
if ! $ESPTOOL --chip esp32s3 --port "$PORT" -b 460800 \
    --before default_reset --after hard_reset write_flash \
    --flash_mode dio --flash_size 16MB --flash_freq 80m \
    0x0     "$HERE/bin/bootloader.bin" \
    0x8000  "$HERE/bin/partition-table.bin" \
    0x10000 "$HERE/bin/launcher.bin"; then
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
