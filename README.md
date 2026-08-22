# ESP32 Hackathon — Shared App Hub

A shared launcher for the **Waveshare ESP32-S3-Touch-AMOLED-1.8**. Everyone writes apps as
Lua scripts; the launcher lists them on the device and runs them. Installing someone
else's app means copying a file — no reflashing, no toolchain, no rebuild.

**Writing an app? You only need [`docs/APP_CONTRACT.md`](docs/APP_CONTRACT.md).**
Apps get touch, swipes and paging, a physical button, shared UI building blocks
(`require("ui")`), text entry (`require("keyboard")`), **offline voice commands**
(`require("voice")` — recognition on the device, no network), timers, and the
Lexend type ramp. [`docs/DESIGN_GUIDE.md`](docs/DESIGN_GUIDE.md) says how to make
it all look right. Everything below is for people working on the launcher itself.

---

## The board

| | |
| --- | --- |
| MCU | ESP32-S3R8 — dual core, 8 MB octal PSRAM, 16 MB flash |
| Display | 1.8" AMOLED, **368 × 448**, QSPI |
| Touch | Capacitive |
| Also on board | QMI8658 IMU · PCF85063A RTC · AXP2101 PMU · ES8311 audio · microSD |

Two hardware revisions exist — **V1** (SH8601 + FT3168, touch at I²C `0x38`) and **V2**
(CO5300 + CST816-family, touch at `0x15`). **The board on this desk is V2**, confirmed from
its own boot log. The Waveshare BSP **≥ 2.0.3 handles both automatically**, so stay on that
version and write against the BSP rather than the panel — then the revision stops mattering
and apps stay portable across everyone's boards.

---

## Launcher setup

### 1. Install ESP-IDF v5.5.5

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.5 --recursive --depth 1 --shallow-submodules \
  https://github.com/espressif/esp-idf.git esp-idf
cd esp-idf && ./install.sh esp32s3
```

Then in every new shell: `. ~/esp/esp-idf/export.sh`

### 2. Build and flash

```bash
cd launcher
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem101 flash monitor
```

Exit the monitor with `Ctrl-]`.

### 3. Install apps

The tools need `pyserial`. Once, per clone:

```bash
python3 -m venv .venv && ./.venv/bin/pip install -r tools/requirements.txt
```

Then the fast path, over USB — no card shuffling, no reboot:

```bash
./.venv/bin/python tools/push.py apps/myapp.lua     # install / update
./.venv/bin/python tools/push.py --list             # what's on the card
./.venv/bin/python tools/push.py --delete old.lua   # remove
```

Or copy `.lua` files to `/apps/` on the microSD card by hand. Either way the
filename becomes the app name in the list.

Start from [`apps/`](apps/): `counter.lua` (the template — copy this),
`stopwatch.lua` (timers + the hero font + the PWR button), `hello_world.lua`.

### 4. Using it

- Tap a row to launch that app.
- **Press BOOT (the top-right button) to return to the launcher.** It is hardware, so it
  works even if an app misbehaves. PWR (bottom right) is the app's button — apps may give
  it a job like lap or play/pause. (Holding PWR ≥6 s still powers the board off.)

---

## Develop without the board

A headless **simulator** (`sim/`) runs apps on your computer and screenshots them — it
compiles the launcher's real Lua↔LVGL bindings against desktop LVGL, so what renders
matches the device. The fast way to iterate on an app, and the CI gate that render-tests
every app on each PR.

```bash
# from the repo root:
(cd sim && ./setup.sh && ./build.sh)
./sim/simctl.py run apps/counter.lua : tap 184 224 : shot out.png
```

It doesn't model hardware quirks (touch imprecision, the watchdog, IMU/voice) — confirm
those on the board. See `sim/README.md`, and `apps/README.md` for the example apps.

## Driving the device without touching it

When you do have the board attached, the launcher speaks a small protocol over the same
USB serial used for flashing — enough to install, launch, drive, and *see* apps with no
hands:

```bash
./.venv/bin/python tools/drive.py run myapp.lua : sleep 1 : tap 184 224 : shot out.png
```

`RUN` / `STOP` / `LIST` / `DELETE` / `TAP x y` / `SWIPE x0 y0 x1 y1 [ms]` /
`PWR` (synthetic button press) / `SHOT` (screenshot → PNG via
`tools/screenshot.py`). Taps inject through a real LVGL input device, so
widgets can't tell them from a finger. This is how the UI and even the voice
module get verified — macOS `say` through the speakers reaches the mic.

---

## Gotchas

Real ones, hit in practice or documented by the vendor. Most cost an hour if you don't
know them.

- **The serial monitor holds the port.** Flashing fails while `monitor` or `screen` is
  attached — detach first.
- **A crash takes the USB port with it.** This board uses the S3's *native* USB, not a
  UART bridge, so firmware that crashes makes `/dev/cu.usbmodem101` vanish. Recovery:
  power off fully, **hold BOOT while powering on** to force download mode, then flash. It
  does *not* auto-exit download mode — power-cycle again afterwards.
- **PWR button: hold ≥ 6 s to power off**, click to power on. Power is under AXP2101
  control, so "off" is real.
- **PSRAM must be configured octal.** `CONFIG_SPIRAM_MODE_OCT=y` is not the IDF default;
  without it the 8 MB PSRAM silently fails to initialise and you lose 8 MB without an
  error message.
- **ESP-IDF build failures after changing dependencies:** delete `build/`,
  `managed_components/`, and `dependencies.lock`, then rebuild.
- **Display brightness** is register `0x51` over QSPI, `0x00`–`0xFF`.
- **`bsp_display_start()` alone leaves the panel dark.** The LCD and touch reset lines are
  on the TCA9554 IO expander, which the BSP never initialises, so the panel stays held in
  reset with no error anywhere. The launcher pulses EXIO1/EXIO2 before display init.
- **`Long filenames on SD card are disabled` is a false warning** — the BSP tests a
  Kconfig choice name that is never defined as a symbol. LFN is genuinely on.
- **Touch is not pixel-accurate**: keep tap targets ≥ ~200×100 or taps get dropped.

---

## Repository layout

| Path | What |
| --- | --- |
| `launcher/` | ESP-IDF project — BSP, LVGL, Lua runtime, app loader |
| `apps/` | Lua apps, one file each; `apps/README.md` indexes them |
| `sim/` | Headless simulator — run and screenshot apps with no board |
| `docs/APP_CONTRACT.md` | The app API — the one doc app authors need |
| `docs/DESIGN_GUIDE.md` | Type, targets, colour, navigation, components |
| `tools/push.py` · `drive.py` · `screenshot.py` | Install, drive, and see the device over USB |
| `CLAUDE.md` | Shared context for Claude Code sessions |

---

## Credits

Built on Espressif's [`lua`](https://components.espressif.com/components/espressif/lua)
component and the LVGL bindings from
[`esp-claw`](https://github.com/espressif/esp-claw), with the
[Waveshare BSP](https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_1_8).
Board examples and prebuilt firmware come from
[waveshareteam/ESP32-S3-Touch-AMOLED-1.8](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8).
