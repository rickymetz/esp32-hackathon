# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

A shared app launcher for the Waveshare ESP32-S3-Touch-AMOLED-1.8. Apps are Lua scripts
loaded from the SD card at runtime — installing one is a file copy, not a reflash.

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
- Display **368 × 448** portrait, QSPI. Brightness = register `0x51`, `0x00`–`0xFF`
- QMI8658 IMU · PCF85063A RTC · AXP2101 PMU · ES8311 audio · microSD (SDMMC)
- Buttons: BOOT = GPIO0 active-low · PWR = EXIO4 on the TCA9554 expander, active-high

Pin map (from the vendor's `pin_config.h`; I²C and SD confirmed by the BSP at runtime):

```
QSPI AMOLED : SDIO0=4  SDIO1=5  SDIO2=6  SDIO3=7  SCLK=11  CS=12
I2C bus     : SDA=15   SCL=14   touch INT=21
ES8311 audio: MCLK=16  BCLK=9   WS=45    DI=10   DO=8    PA=46
SD (SDMMC)  : CLK=2    CMD=1    D0=3
```

`pin_config.h` names the audio pins twice, and the two sets **disagree**:
`I2S_DI_IO=10 / I2S_DO_IO=8` vs `DOPIN=10 / DIPIN=8`. They are named from opposite ends of
the link. Pick one convention; never mix them.

Other I²C addresses: AXP2101 `0x34`, PCF85063 `0x51`, QMI8658 `0x6A`/`0x6B`,
TCA9554 `0x20`, ES8311 `0x18`.

## Build and flash

ESP-IDF **v5.5.5** at `~/esp/esp-idf`. Board is on **`/dev/cu.usbmodem101`**.

```bash
. ~/esp/esp-idf/export.sh          # required in every new shell
cd launcher
idf.py build
idf.py -p /dev/cu.usbmodem101 flash monitor
```

`cmake` and `ninja` come from Homebrew, not from `install.sh`.

**You may build, flash, and monitor freely** — treat it as the normal verify loop.

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
  `/dev/cu.usbmodem101` still exists and enumerates. **No software reset recovers it** —
  `--before usb-reset`, `no-reset-no-sync`, and `watchdog-reset` all fail. Recovery is
  physical: hold PWR ≥6 s to power off → hold BOOT → press PWR → release BOOT. It does
  **not** auto-exit download mode after flashing; power-cycle again or it boots to silence.
- **Reading the console needs an RTS reset pulse.** Opening the port and reading returns
  0 bytes; toggle RTS first (see the pattern used during bring-up).
- **PWR: hold ≥6 s to power off**, click to power on. Power is under AXP2101 control.
- Changed dependencies → delete `build/`, `managed_components/`, `dependencies.lock`.
- `i2c.master: Please check pull-up resistances` on boot is benign on this board.
- **`Warning: Long filenames on SD card are disabled` is a BSP bug, not a real
  problem.** The BSP tests `CONFIG_FATFS_LONG_FILENAMES`, which is a Kconfig *choice*
  name and so is never defined as a symbol — the warning fires unconditionally. Check
  `CONFIG_FATFS_LFN_HEAP` in `sdkconfig` for the truth. LFN **is** enabled here, and it
  matters: without it app filenames are limited to 8.3.
- **Touch is not pixel-accurate.** Measured on hardware: a 240×120 button catches every
  tap, while a 180×56 one dropped roughly half. Size tappable targets ≥ ~200×100. This
  looks exactly like broken event plumbing, so check target size before debugging events.
- **The default 1 MB app partition is too small.** LVGL + Lua + the bindings leave 4%
  free. `partitions.csv` gives the app 4 MB.

## Architecture

- `launcher/` — ESP-IDF app: BSP + LVGL 9.5 + Lua, scans and runs apps
- `apps/` — Lua apps, one file each, installed to `/sdcard/apps/`
- Runtime: `espressif/lua` (official component) + LVGL bindings from `espressif/esp-claw`
  (`components/lua_modules/lua_module_lvgl`)

Each app runs in **its own Lua VM on its own task**, created with the allocator pointed at
PSRAM. The launcher keeps its own LVGL screen and restores it when an app stops, so a
crashed or exited app cannot leave the device unusable.

**Lua event callbacks only queue — they do not fire on their own.** The LVGL event
trampoline in `lua_module_lvgl` enqueues the callback; something must call
`lvgl.process_events()` to drain the queue. The launcher pumps this from the app task so
apps stay declarative: an app builds its UI, wires callbacks, and *returns*. An app that
writes its own `while true` loop freezes the device.

That pump needs **a positive timeout and a yield**. `process_events(0)` returns
immediately when the queue is empty; looping on it starves the idle task and trips the
task watchdog.

Verified on hardware: **Lua 5.5.0 runs**, VM costs ~15.5 KB of PSRAM. Its allocator is
pointed at `MALLOC_CAP_SPIRAM` so apps cannot starve internal DRAM.

Two dead ends, both checked — do not spend time re-discovering them:

- **`lv_binding_lua` does not exist.** No repo, not in the LVGL org, no archived snapshots.
- **`esp-brookesia` cannot be used.** It is Espressif's app OS — launcher, `.bpk` packages,
  app store, Lua/JS/ELF/WASM runtimes, and an official port for *this exact board*. But
  `master` requires **ESP-IDF 6.2 for ESP32-S3, which is unreleased** (newest is
  `v6.1-rc1`), and the IDF-5.5-compatible `release/v0.7` branch has **no `runtime/`
  directory at all**. Revisit after IDF 6.2 ships.

Lua 5.5 note: `lua_newstate()` takes a third `seed` argument, unlike 5.4.

## The app contract

Before changing anything in the app-facing API, read it — five other people are writing
against it, and it is **frozen after the API freeze deadline**.

@docs/APP_CONTRACT.md
