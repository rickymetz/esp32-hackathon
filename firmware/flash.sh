#!/usr/bin/env bash
# Flash the launcher onto a Waveshare ESP32-S3-Touch-AMOLED-1.8.
# Needs Python 3 and a USB-C cable. Does NOT need ESP-IDF.
set -euo pipefail

PORT="${1:-}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ -z "$PORT" ]; then
  PORT=$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null | head -1 || true)
fi
if [ -z "$PORT" ]; then
  echo "No board found. Plug it in, or pass the port: ./flash.sh /dev/cu.usbmodem101" >&2
  exit 1
fi

if ! python3 -c "import esptool" 2>/dev/null; then
  echo "Installing esptool into a local venv..."
  python3 -m venv "$HERE/.venv"
  "$HERE/.venv/bin/pip" install --quiet esptool
  ESPTOOL="$HERE/.venv/bin/python3 -m esptool"
else
  ESPTOOL="python3 -m esptool"
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
