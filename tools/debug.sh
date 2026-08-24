#!/usr/bin/env bash
# JTAG debugging over the SAME USB cable you already flash with.
#
# This board's console is CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y -- the S3's
# built-in USB-Serial/JTAG peripheral, which exposes a CDC serial interface
# AND a JTAG interface on one USB device. So there is no probe to buy and no
# wiring to do; OpenOCD talks to the same thing the serial tools talk to.
#
#   tools/debug.sh openocd     # start the debug server (leave it running)
#   tools/debug.sh gdb         # in a second terminal: attach gdb
#   tools/debug.sh check       # verify openocd can see the chip, then exit
#
# Two things that will bite:
#
#   1. Do not run tools/drive.py, push.py or idf.py monitor against the CDC
#      side while single-stepping. Halting the CPU stops the firmware, so
#      the serial task stops answering and those tools just time out.
#   2. A halted target looks exactly like a crashed one from the host. If
#      the board seems dead after a debug session, `continue` in gdb or
#      quit OpenOCD before reaching for the physical BOOT/PWR recovery.
set -euo pipefail

cd "$(dirname "$0")/.."
. "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1
cd launcher

case "${1:-}" in
    openocd)
        # -c "init; reset halt" is deliberately NOT passed: the default
        # leaves the firmware running so you can attach without disturbing
        # whatever you are trying to observe.
        idf.py openocd --openocd-board board/esp32s3-builtin.cfg
        ;;
    gdb)
        idf.py gdb
        ;;
    check)
        # One-shot probe: start, report the scan chain, exit non-zero if
        # the chip never showed up.
        openocd -f board/esp32s3-builtin.cfg \
                -c "init" -c "esp appimage_offset 0x10000" \
                -c "targets" -c "shutdown" 2>&1 | tail -20
        ;;
    *)
        sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
        exit 2
        ;;
esac
