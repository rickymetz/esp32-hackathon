# ESP32 Hackathon — Shared App Hub

A shared launcher for the **Waveshare ESP32-S3-Touch-AMOLED-1.8**. Everyone writes apps as
Lua scripts; the launcher lists them on the device and runs them. Installing someone
else's app means copying a file — no reflashing, no toolchain, no rebuild.

**Writing an app? You only need [`docs/APP_CONTRACT.md`](docs/APP_CONTRACT.md).**
Everything below is for people working on the launcher itself.

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

Copy app files to `/sdcard/apps/` on the microSD card. The launcher scans that directory
at boot.

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

---

## Repository layout

| Path | What |
| --- | --- |
| `launcher/` | ESP-IDF project — BSP, LVGL, Lua runtime, app loader |
| `apps/template/` | Copy this to start an app |
| `docs/APP_CONTRACT.md` | The app API — the one doc app authors need |
| `CLAUDE.md` | Shared context for Claude Code sessions |

---

## Credits

Built on Espressif's [`lua`](https://components.espressif.com/components/espressif/lua)
component and the LVGL bindings from
[`esp-claw`](https://github.com/espressif/esp-claw), with the
[Waveshare BSP](https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_1_8).
Board examples and prebuilt firmware come from
[waveshareteam/ESP32-S3-Touch-AMOLED-1.8](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8).
