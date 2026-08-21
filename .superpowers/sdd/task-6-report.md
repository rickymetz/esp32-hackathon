# Task 6 Report: Prebuilt firmware and `flash.sh`

## Summary

Built the launcher from current `main` (`4560a9a`), committed the three
binaries to `firmware/bin/`, and shipped `firmware/flash.sh` +
`firmware/README.md`. `flash.sh` was tested end-to-end in an environment
with no ESP-IDF at all and required one real fix over the brief's verbatim
script (see "Deviation" below) to actually work.

## Build

```
$ . ~/esp/esp-idf/export.sh > /dev/null
$ cd launcher && idf.py build
...
-- App "launcher" version: 4560a9a
...
Bootloader binary size 0x5710 bytes. 0x28f0 bytes (32%) free.
...
launcher.bin binary size 0xf7e30 bytes. Smallest app partition is 0x400000 bytes. 0x3081d0 bytes (76%) free.

Project build complete.
```

Built clean from `main` at `4560a9a` (no uncommitted changes anywhere in the repo before or after).

## Deviation from the brief (and why)

The brief's `flash.sh` script was used verbatim except for two lines, both
found by actually running the "no ESP-IDF" verification step rather than
trusting the brief's syntax:

1. **`ESPTOOL="$HERE/.venv/bin/esptool"` → `"$HERE/.venv/bin/python3 -m esptool"`**
   On a bare `PATH=/usr/bin:/bin:/usr/local/bin` environment, `python3`
   resolves to macOS's bundled Python 3.9.6, and `pip install esptool` on
   Python 3.9 resolves to **esptool 4.12.0**, whose installed console
   scripts are `esptool.py`, `espefuse.py`, etc. — there is **no bare
   `esptool` binary** in that version. `$HERE/.venv/bin/esptool` didn't
   exist, so the flash failed with "No such file or directory" before
   ever touching the board. Invoking via `python3 -m esptool` sidesteps
   entry-point naming entirely and works on both old and new esptool.

2. **Hyphenated flags (`write-flash`, `--flash-mode`, `default-reset`,
   `hard-reset`) → underscored (`write_flash`, `--flash_mode`,
   `default_reset`, `hard_reset`)**
   esptool 4.12.0 (installed on the Python-3.9 path above) only
   understands the underscored spelling — `--before default-reset` is a
   hard error ("invalid choice"). The newer esptool in my own repo venv
   (`./.venv`, Python 3.14, esptool 5.3.1) accepts the underscored
   spelling too, as a working (if deprecation-warned) alias. Underscored
   syntax is the only spelling that works across both, so that's what
   ships. This matters because the five recipients' machines will have
   whatever system Python they have — old system Pythons on macOS are
   exactly the Python-3.9-ish case that resolves the older esptool.

No other changes from the brief's script, README, or commit message intent.

## Verification

### 1. `firmware/bin/` — three binaries, freshly built from current `main`

```
$ stat -f "%N %z bytes" firmware/bin/*.bin
firmware/bin/bootloader.bin 22288 bytes
firmware/bin/launcher.bin 1015344 bytes
firmware/bin/partition-table.bin 3072 bytes
```

Total `firmware/bin/`: **1.0 MB** (`du -sh`). `launcher.bin` ~1015 KB, matching the brief's
"around 1 MB" expectation. This total is fine to commit to the repo as-is.

### 2. `flash.sh` in an environment with no ESP-IDF at all

```
$ env -i HOME="$HOME" PATH=/usr/bin:/bin:/usr/local/bin bash ./firmware/flash.sh /dev/cu.usbmodem101
Installing esptool into a local venv...
WARNING: You are using pip version 21.2.4; however, version 26.0.1 is available.
...
Flashing /dev/cu.usbmodem101 ...
esptool.py v4.12.0
Serial port /dev/cu.usbmodem101
Connecting...
Chip is ESP32-S3 (QFN56) (revision v0.2)
Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)
Crystal is 40MHz
USB mode: USB-Serial/JTAG
MAC: 28:84:85:3b:7c:c0
Uploading stub...
Running stub...
Stub running...
Changing baud rate to 460800
Changed.
Configuring flash size...
Flash will be erased from 0x00000000 to 0x00005fff...
Flash will be erased from 0x00008000 to 0x00008fff...
Flash will be erased from 0x00010000 to 0x00107fff...
SHA digest in image updated
Compressed 22288 bytes to 14243...
Writing at 0x00000000... (100 %)
Wrote 22288 bytes (14243 compressed) at 0x00000000 in 0.2 seconds (effective 771.9 kbit/s)...
Hash of data verified.
Compressed 3072 bytes to 119...
Writing at 0x00008000... (100 %)
Wrote 3072 bytes (119 compressed) at 0x00008000 in 0.0 seconds (effective 615.5 kbit/s)...
Hash of data verified.
Compressed 1015344 bytes to 579893...
Writing at 0x00010000... (2 %)
...
Writing at 0x00105231... (100 %)
Wrote 1015344 bytes (579893 compressed) at 0x00010000 in 5.7 seconds (effective 1437.4 kbit/s)...
Hash of data verified.

Leaving...
Hard resetting via RTS pin...

Done. Put your .lua apps in /apps on the microSD card.
```

Installed esptool into `firmware/.venv` from scratch (venv did not exist beforehand) and flashed successfully.

### 3. Board boots to a working launcher after that flash

```
$ ./.venv/bin/python tools/read_serial.py 8
...
I (647) app_init: Project name:     launcher
I (647) app_init: App version:      4560a9a
...
=== ESP32-S3-Touch-AMOLED-1.8 Launcher ===
LVGL 9.5.0 / Lua 5.5.0
I (725) launcher: panel reset released via IO expander
...
I (943) ESP32-S3-Touch-AMOLED-1.8: Touch CST816S 0x15 found
...
I (1019) app_registry: SD card mounted at /sdcard
I (1021) app_registry: found app 'Counter' (/sdcard/apps/counter.lua)
I (1021) app_registry: found app 'Hello world' (/sdcard/apps/hello_world.lua)
I (1021) app_registry: found app 'Pushed' (/sdcard/apps/pushed.lua)
I (1022) app_registry: found app 'Racetest' (/sdcard/apps/racetest.lua)
I (1022) app_registry: found app 'Tick test' (/sdcard/apps/tick_test.lua)
I (1022) app_registry: found app 'Timer reuse' (/sdcard/apps/timer_reuse.lua)
I (1023) app_registry: found app 'Runaway bare' (/sdcard/apps/runaway_bare.lua)
I (1024) app_registry: found app 'Runaway pcall' (/sdcard/apps/runaway_pcall.lua)
I (1024) app_registry: found app 'Runaway coro' (/sdcard/apps/runaway_coro.lua)
I (1024) app_registry: found app 'Trim check' (/sdcard/apps/trim_check.lua)
I (1025) app_registry: found app 'Hook bypass' (/sdcard/apps/hook_bypass.lua)
I (1025) app_registry: found app 'Broken' (/sdcard/apps/broken.lua)
I (1026) app_registry: found app 'Deep error' (/sdcard/apps/deep_error.lua)
I (1027) app_registry: 13 app(s) found
I (1165) launcher: ready: 13 app(s), internal free=205919 psram free=8207380
I (1165) main_task: Returned from app_main()
```

Booted clean, panel/touch/SD all initialized, **13 apps found** — matches the 11 `.lua`
files present in `/Users/rickmetzger/code/personal/esp32-hackathon/apps/` plus two extra
(`pushed.lua`, `racetest.lua`) already on the board's SD card from prior session work, not
part of this repo. Expected and correct — the app registry just enumerates whatever `.lua`
files are on the card.

### 4. `flash.sh` with no port argument — auto-detect

```
$ env -i HOME="$HOME" PATH=/usr/bin:/bin:/usr/local/bin bash ./firmware/flash.sh
Installing esptool into a local venv...
...
Flashing /dev/cu.usbmodem101 ...
esptool.py v4.12.0
Serial port /dev/cu.usbmodem101
Connecting...
Chip is ESP32-S3 (QFN56) (revision v0.2)
...
Wrote 1015344 bytes (579893 compressed) at 0x00010000 in 5.6 seconds (effective 1445.8 kbit/s)...
Hash of data verified.

Leaving...
Hard resetting via RTS pin...

Done. Put your .lua apps in /apps on the microSD card.
```

Auto-detected `/dev/cu.usbmodem101` via the `ls /dev/cu.usbmodem* /dev/ttyACM*` fallback and flashed successfully with no port argument.

### 5. Failure path — nonexistent port prints recovery guidance, not a raw traceback

```
$ env -i HOME="$HOME" PATH=/usr/bin:/bin:/usr/local/bin bash ./firmware/flash.sh /dev/cu.doesnotexist
Installing esptool into a local venv...
...
Flashing /dev/cu.doesnotexist ...
esptool.py v4.12.0
Serial port /dev/cu.doesnotexist

A fatal error occurred: Could not open /dev/cu.doesnotexist, the port is busy or doesn't exist.
([Errno 2] could not open port /dev/cu.doesnotexist: [Errno 2] No such file or directory: '/dev/cu.doesnotexist')

Hint: Check if the port is correct and ESP connected


Flashing failed.

If the board previously ran an app that crashed, its USB is wedged and no
software reset can recover it. Do this by hand:

  1. Hold PWR for at least 6 seconds to power off.
  2. Hold BOOT down and keep holding it.
  3. Press PWR to power on, then release BOOT.
  4. Run this script again.
  5. Afterwards power-cycle again -- it does not leave download mode on its own.

Also check nothing else is holding the port (a serial monitor will block it).
EXIT=1
```

esptool's own error surfaces (not swallowed), followed immediately by the
script's own recovery block. Exit code 1, no bash traceback. The recovery
message covers both causes named in the task brief: the physical
BOOT/PWR power-cycle sequence for a USB-wedged board, and a serial monitor
holding the port.

## Other notes

- `/dev/cu.usbmodem101` transiently disappeared from `ls` once during this
  session and reappeared within a couple of seconds on retry — matches the
  brief's warning that this is a known false alarm, not a real disconnect.
- `firmware/.venv/` needs no new `.gitignore` entry: the existing root-level
  `.venv/` pattern (no leading slash) already matches directories named
  `.venv` anywhere in the tree, confirmed with
  `git check-ignore -v firmware/.venv/foo` → matched by `.gitignore:9:.venv/`.
  `git status` shows `firmware/.venv/` does not appear as untracked after a
  full `flash.sh` run recreates it, and it was deleted after each test run
  regardless.
- Board's final state at the end of this task: freshly flashed, verified
  booting to a working launcher with the SD card mounted and 13 apps
  enumerated. Left in that state — no unflashed or hung window.

---

## Post-review fixes (5 findings) — verification log

Commit under test: fixes to `firmware/flash.sh`, `firmware/README.md`, new
`firmware/bin/BUILD_INFO`. Board: `/dev/cu.usbmodem101`.

### 1. Stripped environment, fresh venv path (real)

```
$ env -i HOME="$HOME" PATH=/usr/bin:/bin:/usr/local/bin bash ./flash.sh /dev/cu.usbmodem101
Installing esptool into a local venv...
Using esptool 4.12.0 from /Users/rickmetzger/code/personal/esp32-hackathon/firmware/.venv (freshly installed)
Flashing build 01cf670 from 2026-08-21
Flashing /dev/cu.usbmodem101 ...
esptool.py v4.12.0
...
Hard resetting via RTS pin...

Done. Put your .lua apps in /apps on the microSD card.
EXIT_CODE=0
```

### 2. Previously-untested "already importable" branch (real)

Used a PATH shim resolving `python3` to a real interpreter with esptool 5.3.1
already installed (this repo's top-level `.venv`), with `firmware/.venv`
absent, in a stripped `env -i` environment:

```
$ env -i HOME="$HOME" PATH="/tmp/fakebin:/usr/bin:/bin:/usr/local/bin" bash ./flash.sh /dev/cu.usbmodem101
Using esptool 5.3.1 from /tmp/fakebin/python3 (already installed)
Flashing build 01cf670 from 2026-08-21
Flashing /dev/cu.usbmodem101 ...
esptool v5.3.1
...
Hard resetting via RTS pin...

Done. Put your .lua apps in /apps on the microSD card.
EXIT_CODE=0
---venv exists?---
".venv": No such file or directory (os error 2)
correctly no venv created
```

Confirms: the branch executes, uses the pre-existing esptool, prints which
one and where from, does not create `firmware/.venv`, and flashes
successfully. Note: an earlier attempt using a plain symlink to the venv's
python binary silently failed to import esptool (venv site-packages
resolution issue) and fell through to a fresh install undetected — switching
the shim to an exec wrapper script fixed this and is what produced the above
real result.

### 3. Version floor (>= 4.0) — real, both branches

Installed a genuinely old esptool for this test:

```
$ python3 -m venv /tmp/oldesptool_venv
$ /tmp/oldesptool_venv/bin/pip install --quiet "esptool<4.0"
$ /tmp/oldesptool_venv/bin/python3 -c "import esptool; print(esptool.__version__)"
3.3.3
```

Pointed the `python3` PATH shim at that interpreter and ran flash.sh in a
stripped environment with `firmware/.venv` absent:

```
System esptool is 3.3.3, need >= 4.0 -- installing a newer one into a local venv.
Installing esptool into a local venv...
Using esptool 5.3.1 from /Users/rickmetzger/code/personal/esp32-hackathon/firmware/.venv (freshly installed)
Flashing build 01cf670 from 2026-08-21
Flashing /dev/cu.usbmodem101 ...
esptool v5.3.1
...
Done. Put your .lua apps in /apps on the microSD card.
```

`.venv/bin/python3 -c "import esptool; print(esptool.__version__)"` afterward
confirmed 5.3.1 in the fresh venv. This is a REAL exercise of both branches —
the old-esptool detection and the fall-through-to-venv behavior — not a
stubbed/simulated version string. The "ok" (>=4.0) branch was exercised for
real in check 2 above (esptool 5.3.1) and check 1 (freshly installed 4.12.0).

### 4. Multiple boards found — simulated (logic-identical extract)

`/dev` entries cannot be fabricated in this sandbox without root, so the
port-selection block (identical logic, copied verbatim from flash.sh) was
run against two dummy files matching the glob pattern:

```
--- stdout only ---
SELECTED_PORT=/tmp/fakeports/cu.usbmodemAAA
--- stderr only ---
Multiple boards found; using /tmp/fakeports/cu.usbmodemAAA
Other ports seen, not used: /tmp/fakeports/cu.usbmodemBBB
```

Confirms the chosen port and the alternatives go to stderr, stdout stays
clean. This part is SIMULATED (same code, fake input), not run against real
/dev nodes.

### 5. venv/pip failure guidance (real)

```
$ env -i HOME="$HOME" PATH=/usr/bin:/bin:/usr/local/bin \
    PIP_INDEX_URL=http://127.0.0.1:1/simple PIP_RETRIES=0 PIP_TIMEOUT=3 \
    bash ./flash.sh /dev/cu.usbmodem101
Installing esptool into a local venv...
ERROR: Could not find a version that satisfies the requirement esptool>=4.0 (from versions: none)
ERROR: No matching distribution found for esptool>=4.0
WARNING: You are using pip version 21.2.4; however, version 26.0.1 is available.
You should consider upgrading via the '.../firmware/.venv/bin/python3 -m pip install --upgrade pip' command.

Could not install esptool automatically.

Likely causes: no network connection, a proxy blocking PyPI, or this
Python is missing the built-in "venv" module.

Manual fallback:
  1. python3 -m pip install --user esptool
  2. Run this script again.
EXIT_CODE=1
```

Real pip failure via an unreachable index, no raw traceback, script exits 1
after printing guidance (in addition to pip's own error text, which is
expected/left visible).

### 6. BUILD_INFO

```
$ cat firmware/bin/BUILD_INFO
commit=01cf670
date=2026-08-21
$ git log -1 --format=%h -- firmware/bin/launcher.bin firmware/bin/bootloader.bin firmware/bin/partition-table.bin
01cf670
$ git log -1 --format=%cs -- firmware/bin/launcher.bin
2026-08-21
```

Matches the commit that staged the current binaries. `flash.sh` prints
`Flashing build 01cf670 from 2026-08-21` before every flash (see runs above).

### Final clean flash + board health check

Re-flashed cleanly (no truncation, no faults) after the version-floor test
to guarantee end state:

```
Using esptool 4.12.0 from .../firmware/.venv (freshly installed)
Flashing build 01cf670 from 2026-08-21
...
Hard resetting via RTS pin...

Done. Put your .lua apps in /apps on the microSD card.
EXIT_CODE=0
```

Serial console after an RTS-pulse reset (pyserial from the repo `.venv`,
`pip install pyserial` there) shows a clean boot:

```
I (1020) app_registry: SD card mounted at /sdcard
I (1022) app_registry: found app 'Counter' (/sdcard/apps/counter.lua)
... (13 apps listed) ...
I (1027) app_registry: 13 app(s) found
I (1165) launcher: ready: 13 app(s), internal free=205919 psram free=8207380
I (1166) main_task: Returned from app_main()
```

Board ends this task on a healthy, working launcher with 13 apps found.
