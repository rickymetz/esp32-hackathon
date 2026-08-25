# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

A shared app launcher for the Waveshare ESP32-S3-Touch-AMOLED-1.8. Apps are Lua scripts
loaded from the SD card at runtime — installing one is a file copy, not a reflash.

**Architecture lives in `codemaps/`** — regenerated, loaded at session start, and the
place to look for *what exists where*:

| Map | Covers |
| --- | --- |
| `codemaps/architecture.md` | task model, lock order, module graph, app lifecycle, build |
| `codemaps/firmware.md` | per-file breakdown of `launcher/main` and `components/` |
| `codemaps/data.md` | structs, every limit, the pin map, measured figures |
| `codemaps/tooling.md` | `tools/` and `sim/`, and the verification recipes |

This file is the other half: the **rules and constraints** that are not derivable from
reading the code, and that cost an hour each when rediscovered. Regenerate the maps with
`/cc-codemaps:update-codemaps`; they are generated, so edit this file instead.

## Hardware

**Board revision is V2** — confirmed on hardware, not assumed:

```
co5300: version 2.1.0              → CO5300 display
Touch CST816S 0x15 found           → CST816-family touch at I2C 0x15
```

The V1 board uses SH8601 + FT3168 at `0x38` instead. **Always go through the BSP**
(`waveshare/esp32_s3_touch_amoled_1_8` ^2.0.3), which depends on both touch drivers and
binds the right one at runtime. Writing directly against CO5300/CST816 hardcodes the
revision and breaks on V1 boards.

- ESP32-S3R8, rev v0.2 — dual core, **8 MB octal PSRAM**, **16 MB** flash
- Display **368 × 448** portrait, QSPI
- QMI8658 IMU · PCF85063A RTC · AXP2101 PMU · ES8311 audio · microSD (SDMMC)
- Buttons: BOOT = GPIO0 active-low · PWR = EXIO4 on the TCA9554 expander, active-high

**Pin map, I²C addresses and expander lines: `codemaps/data.md`.** One warning worth
repeating here, because mixing them silently produces a dead microphone:
`pin_config.h` names the audio pins twice and the two sets **disagree**
(`I2S_DI_IO=10 / I2S_DO_IO=8` vs `DOPIN=10 / DIPIN=8`). They are named from opposite ends
of the link. Pick one convention; never mix them.

## Build and flash

ESP-IDF **v5.5.5** at `~/esp/esp-idf`.

**Do not hardcode the serial port.** It is usually `/dev/cu.usbmodem101`, but this
board's native USB re-enumerates on its own and comes back under a *different* name
(`/dev/cu.usbmodem1101` was seen mid-session). Every tool in `tools/` globs
`/dev/cu.usbmodem*` for this reason; only `idf.py` needs to be told, so resolve it:

```bash
. ~/esp/esp-idf/export.sh          # required in every new shell
export PORT=$(printf '%s\n' /dev/cu.usbmodem* | head -1)   # glob, not ls: ls colourises
cd launcher
idf.py build
idf.py -p $PORT flash monitor
```

`cmake` and `ninja` come from Homebrew, not from `install.sh`.

**You may build, flash, and monitor freely** — treat it as the normal verify loop.

**Verify UI work with the drive harness, not by asking a human.** The launcher
speaks `SHOT` / `TAP x y` / `SWIPE x0 y0 x1 y1 [ms]` / `LIST` / `DELETE` /
`MEM` / `STATS` / `PING` / `BRIGHT` over
serial next to RUN/STOP; `tools/drive.py` chains them and `tools/screenshot.py`
decodes SHOT to a PNG (~1.6 s per frame):

```bash
./.venv/bin/python tools/push.py tests/fixtures/ui_test.lua      # fixtures live here
./.venv/bin/python tools/drive.py run ui_test.lua : sleep 1 : tap 184 224 : shot out.png
```

Taps inject through a real LVGL indev — widgets cannot tell them from a
finger — and queue with an enforced release gap, so back-to-back taps are safe.

**Both audio directions close without a human.** macOS `say` through the
speakers reaches the board's microphone (this is how `voice` was verified),
and `tools/hear.py` records the Mac microphone and runs a Goertzel filter to
confirm the board's speaker is actually sounding:

```bash
./.venv/bin/python tools/hear.py 880      # ratio ~1 = silent, tens+ = a real tone
```

Verified this way: commanding 880 Hz measured 12.4x at 880 and 0.45x at 1568;
commanding 1568 Hz measured 216x at 1568 and 0.45x at 880 — the detected
frequency tracks the commanded one, so it is the board and not the room.
`hear.py` needs microphone permission for the terminal, and without it ffmpeg
hangs rather than failing. It records the room, so keep captures short.

`sdkconfig.defaults` is not optional. `CONFIG_SPIRAM_MODE_OCT=y` in particular is **not**
the IDF default, and without it the 8 MB PSRAM silently fails to initialise with no error.

## BSP gap: the panel stays dark without an expander reset

**`bsp_display_start()` on its own leaves the screen black, with no error anywhere.**
Every software signal looks healthy — panel created, `disp_on_off(true)`, brightness OK,
touch found, LVGL flushing — and the panel is still dark. Waveshare's own prebuilt
`00_bsp_quickstart` has the same problem, so this is the BSP, not your code.

Cause: on this board `BSP_LCD_RST`, `BSP_LCD_TOUCH_RST`, and `BSP_LCD_BACKLIGHT` are all
`GPIO_NUM_NC`. The reset lines hang off the **TCA9554 IO expander**, and
`bsp_display_start()` never initialises it — so the panel sits held in reset.

Fix, which must run **before** `bsp_display_start()` (see `launcher/main/launcher_main.c`):
pulse **EXIO1 (LCD reset) and EXIO2 (touch reset)** low → 20 ms → high via
`bsp_io_expander_init()`. Also on the expander: **EXIO4 = PWR button, EXIO5 = PMU IRQ**,
both inputs.

## Gotchas

These cost an hour each if you don't know them. Most were hit for real in this repo.

- **The monitor holds the port.** Flashing fails while `monitor`/`screen` is attached.
- **A crash takes USB with it.** This board uses the S3's *native* USB, not a UART bridge,
  so a hung app makes flashing fail with `No serial data received` even though
  the port still exists and enumerates. **No software reset recovers it** —
  `--before usb-reset`, `no-reset-no-sync`, and `watchdog-reset` all fail. Recovery is
  physical: hold PWR ≥6 s to power off → hold BOOT → press PWR → release BOOT. It does
  **not** auto-exit download mode after flashing; power-cycle again or it boots to silence.
- **Reading the console needs an RTS reset pulse.** Opening the port and reading returns
  0 bytes; toggle RTS first (see the pattern used during bring-up).
- **PWR: hold ≥6 s to power off**, click to power on. Power is under AXP2101 control.
- Changed dependencies → delete `build/`, `managed_components/`, `dependencies.lock`.
- `i2c.master: Please check pull-up resistances` on boot is benign on this board.
- **Any component that includes `lua.h` must agree with the vendored Lua core on
  `LUA_32BITS`**, or `lua_Integer`/`lua_Number` sizes mismatch and it aborts inside
  `luaL_checkversion_`. Set build-wide in `launcher/CMakeLists.txt` via
  `idf_build_set_property(COMPILE_DEFINITIONS "LUA_32BITS=1" APPEND)` — don't re-add it
  per-component.
- **`Warning: Long filenames on SD card are disabled` is a BSP bug, not a real
  problem.** The BSP tests `CONFIG_FATFS_LONG_FILENAMES`, which is a Kconfig *choice*
  name and so is never defined as a symbol — the warning fires unconditionally. Check
  `CONFIG_FATFS_LFN_HEAP` in `sdkconfig` for the truth. LFN **is** enabled here, and it
  matters: without it app filenames are limited to 8.3.
- **Touch is not pixel-accurate.** Measured on hardware: a 240×120 button catches every
  tap, while a 180×56 one dropped roughly half. Size tappable targets ≥ ~200×100. This
  looks exactly like broken event plumbing, so check target size before debugging events.
- **MultiNet's `detect()` nearly saturates a core.** A single capture under the
  10 s task watchdog survives; chained captures (voice.spell) starved the idle
  task and rebooted the board until a 2 ms per-chunk `vTaskDelay` was added.
- **The MultiNet command table binds to the model instance passed to
  `esp_mn_commands_alloc`.** Destroy that instance and the vocabulary silently
  points at dead state — recognition "works" against the wrong command set.
  One persistent instance, `clean()`ed between captures, never destroyed.
- **cap_lua opens modules in registration order.** Embedded-Lua modules
  (`ui`, `keyboard`) `require()` other modules at load time — anything they
  depend on (lvgl, timer, voice) must be registered first in `app_main`.
- **The theme font is set at runtime, not via Kconfig.** LVGL's
  `LV_FONT_DEFAULT` choice only offers bundled faces, so Lexend is applied with
  `lv_theme_default_init(...)` under `bsp_display_lock` in `app_main`;
  Montserrat 14 stays compiled solely so the macro resolves. Custom fonts must
  be generated with `--no-compress` — compressed bitmaps render blank because
  `LV_USE_FONT_COMPRESSED` is off.
- **The default 1 MB app partition is too small.** LVGL + Lua + the bindings leave 4%
  free. `partitions.csv` gives the app 4 MB.

## Architecture — the rules

Structure, module graph and lifecycle are in `codemaps/architecture.md` and
`codemaps/firmware.md`. What follows is only what you can get *wrong*.

One thing those maps predate: **watch faces are not in `apps/`** — they are part of the
shell, in `launcher/main/launcher_face.c`. `apps/` holds only real apps, each a flat
`apps/<name>.lua` or a folder `apps/<name>/main.lua` shipping its own `icon.bin`
(see `docs/SD_CARD_APPS.md`); test fixtures live in `tests/fixtures/`.


Each app runs in **its own Lua VM on its own task**, allocator pointed at PSRAM. The
launcher keeps its own LVGL screen and restores it when an app stops, so a crashed or
exited app cannot leave the device unusable.

**Lua event callbacks only queue — they do not fire on their own.** The LVGL event
trampoline in `lua_module_lvgl` enqueues the callback; something must call
`lvgl.process_events()` to drain the queue. The launcher pumps this from the app task so
apps stay declarative: an app builds its UI, wires callbacks, and *returns*. An app that
writes its own `while true` loop freezes the device.

That pump needs **a positive timeout and a yield**. `process_events(0)` returns
immediately when the queue is empty; looping on it starves the idle task and trips the
task watchdog.

**LOCK ORDER: the display lock is always outermost.** esp_lvgl_port holds it across the
whole of `lv_timer_handler()`, so every LVGL event callback runs inside it and then takes
`s_app_mutex`. Any other task that needs both must use that same order — or take neither
and post the work to the LVGL task with `lv_async_call()`. Taking `s_app_mutex` first and
then blocking on the display lock is an AB-BA deadlock with any concurrent tap, and
because both tasks block *cleanly* no watchdog fires: the board just stops, and recovery
is the physical PWR/BOOT dance. This shipped once. Do not reintroduce it.

**Home is the watch face, not the app list.** The device boots to a face from
`launcher_face.c` — pure LVGL, no card, no Lua VM, so it is always available and is the
fallback if a user-configured home app ever fails. The app list is a surface you
navigate to.

**Five faces live in C**: Digital, Analog, Rings, Words, Minimal — ported from the former
`apps/faces.lua` and `apps/clock.lua`, which between them were three competing copies of
"the watch face". **Swipe left/right on home cycles them**; the choice is saved to NVS.
Faces are built once and then *mutated* per tick — the analog dial alone is 60+ tick
lines plus three hands, so rebuilding it to move a second hand is out of the question.
The tick runs at 250 ms for faces with a second hand and 1 s otherwise.

**Timezone lives in exactly one place.** NTP sets the RTC in **UTC**, so the shell
applies an offset (minutes east, in NVS) when it reads the clock, rolling the date
properly. `faces.lua` carried a warning about two copies of that logic disagreeing and
showing the UTC date beside a local time; there is now one copy, in `shift_local()`.

**BOOT (GPIO0, top right, active low) is the only navigation control**, and it is a
three-way toggle:

| From | BOOT goes to |
| --- | --- |
| a running app | home (the app is stopped first) |
| home (face) | the app list |
| the app list | home |

A direct GPIO read, so unlike the old PWR path it has no I²C dependency and survives a
wedged bus. Deliberately hardware: no app can consume it or paint over it, so a
misbehaving app is always escapable. Polled every 20 ms in `button_poll_task` with a
two-sample debounce; it requests stop **unconditionally** rather than gating on "is an
app running", which is what broke the first version — the `s_app_task` read afterwards
only decides whether the press *also* toggles the shell surface.

The face repaints on a 5 s LVGL timer that is paused while the app list is up. Sampling
faster than the minute it displays is deliberate: a 60 s timer against a 1 Hz clock
drifts and skips whole minutes — the same trap `docs/APP_CONTRACT.md` warns apps about.

**PWR (EXIO4 on the expander, active high) belongs to apps** via `require("button")` —
pressed/released/long_pressed(2 s). Holding PWR ≥6 s still powers off — that is the
AXP2101 below us, which is also why the contract bans destructive actions on it.

Verified on hardware: launching and exiting an app repeatedly returns PSRAM to exactly
the same free figure, so the launch/exit cycle does not leak.

Lua 5.5 note: `lua_newstate()` takes a third `seed` argument, unlike 5.4.

Three dead ends, all checked — do not spend time re-discovering them:

- **`lv_binding_lua` does not exist.** No repo, not in the LVGL org, no archived snapshots.
- **`esp-brookesia` cannot be used.** It is Espressif's app OS — launcher, `.bpk` packages,
  app store, Lua/JS/ELF/WASM runtimes, and an official port for *this exact board*. But
  `master` requires **ESP-IDF 6.2 for ESP32-S3, which is unreleased** (newest is
  `v6.1-rc1`), and the IDF-5.5-compatible `release/v0.7` branch has **no `runtime/`
  directory at all**. Revisit after IDF 6.2 ships.
- **The usb_serial_jtag driver does not speed up `SHOT`.** Installing it and calling
  `usb_serial_jtag_vfs_use_driver()` looks like the obvious fix for the per-byte console
  write. Measured on hardware: **4–5× worse** (1.5 s → 7–10 s). Do not re-attempt without
  measuring.

## The app contract

Before changing anything in the app-facing API, read it — five other people are writing
against it, and it is **frozen after the API freeze deadline**.

@docs/APP_CONTRACT.md
