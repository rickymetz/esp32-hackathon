# Launcher SDK — Design

**Date:** 2026-08-20
**Status:** Approved, ready for implementation planning

## Context

The shared app launcher works end to end: apps are Lua files on the SD card, the launcher
lists them, tapping one runs it in its own VM, and PWR returns to the list. Verified on
hardware with no memory leak across launch/exit cycles.

What exists today is display and touch only. This design covers what to build in the two
weeks before the hackathon so that 5–6 people can each write apps against a stable SDK.

### Fixed constraints

| | |
| --- | --- |
| Participants | 5–6, mixed embedded experience |
| Boards | **One each** — no sharing |
| Timeline | ~2 weeks of prep |
| Board | Waveshare ESP32-S3-Touch-AMOLED-1.8 (V2), 368×448, 8 MB PSRAM, 16 MB flash |

**The board count is the load-bearing constraint.** Because everyone has hardware, app
authors never need ESP-IDF — they flash a prebuilt binary once and then only copy files.
It also means the Emscripten simulator (insurance against sharing a single board) is
wasted effort and is explicitly not being built.

### Wanted app categories

Motion toys · clocks and watch faces · pedometers and workout counters · games and demos ·
sound · battery utilities · network apps · LLM pass-throughs.

Tailscale was raised and **dropped** — no official ESP32 client, and a real one means
WireGuard plus the coordination protocol. Not committed to, so not built.

## Approach

Distribution first, then cheap capability wins, then networking as a contained chunk.

Rejected: network-first (highest ceiling, but if it slips the cheap wins never land and
people are left with display-only apps) and build-on-demand (lowest waste, but people hit
walls mid-build and block on one person).

The chosen order front-loads what blocks five people, banks the high-value-per-hour APIs
early, and quarantines the risky work where slipping is survivable.

## Architecture

### Capability modules

One Lua module per capability, each an ESP-IDF component under `launcher/components/`,
registered through the existing `cap_lua` shim and reached with `require`.

Reuse is uneven, and that drives the estimates:

| Capability | Source | Cost |
| --- | --- | --- |
| `store`, `json`, `system` | Vendor from `espressif/esp-claw`, zero dependencies | Cheap |
| `imu` | `waveshare/qmi8658` v2.0.1 driver + hand-written binding | Medium |
| `rtc`, `battery` | No component exists; write I²C drivers for PCF85063 / AXP2101 | Medium |
| `audio` | BSP already provides `bsp_audio_init`, speaker and mic codec init | Medium |
| `net` | Nothing reusable; `esp_wifi` + `esp_http_client` + TLS | Large |

**esp-claw's `lua_module_imu` is the wrong chip** — it supports BMI270 and ICM42670, not
the QMI8658 on this board. Its `lua_module_audio` pulls `esp_audio_codec`, `esp_asrc` and
`esp-dsp` for MP3 decode and resampling we do not need; the BSP path is far lighter.

### Launcher owns `lvgl.init()`

Apps currently must call `lvgl.init()` or fail confusingly. The launcher will call it
before running the app, so apps begin at `local scr = lvgl.screen()`.

This preserves the isolation property that matters: the display session still belongs to
the app's VM, so exiting still deletes the app's screen and every widget parented to it.

### Error convention

Modules return `nil, "message"` rather than raising. A missing SD card, absent Wi-Fi, or
unavailable sensor degrades instead of killing the app. Hardware that is not present
reports itself unavailable.

### API freeze

The surface below is frozen once people begin writing apps. Later additions are additive
only — no signature changes.

## Phases

### Phase 1 — Unblock five people (~2 days)

Nothing else matters until this is done.

- Git remote and repo hygiene — **there is currently no remote; nobody can clone**
- `firmware/` with prebuilt binaries plus `flash.sh` that pip-installs `esptool` and
  flashes. App authors never install ESP-IDF; only the launcher maintainer does. No CI
  initially — the maintainer refreshes binaries. Add CI only if that becomes annoying.
- **Serial file push**: launcher listens on the existing USB serial; `push.py` sends a
  `.lua` straight to the SD card. Needs framing so it does not collide with log output.
- **Refresh button** — rescan apps without rebooting
- **Scrolling app list** — the current list breaks past roughly five apps
- **App errors shown on screen**, not only on serial

Serial push exists because the alternative is: eject SD → adapter → Mac → copy → back →
reboot. Roughly 30 seconds, repeated 20+ times per app author per day. It is the worst
friction in the current design and it hits everyone.

App sharing is the repo: apps live in `apps/`, people push theirs, others `git pull`.

### Phase 2 — Cheap wins (~4 days)

Unlocks motion toys, clocks, pedometers and workout counters.

```lua
store.get(k) / store.set(k, v)     -- namespaced per app, survives reboot
imu.accel() -> x, y, z             -- g
imu.gyro()  -> x, y, z             -- deg/s
imu.steps() -> count               -- QMI8658 hardware pedometer
rtc.now()   -> {year, month, day, hour, min, sec, wday}
battery.percent() / .volts() / .charging()
json.encode(t) / json.decode(s)
```

The QMI8658's **hardware pedometer** makes step counters and workout apps nearly free
rather than a signal-processing exercise.

### Phase 3 — Audio (~1–1.5 days)

```lua
audio.tone(freq_hz, ms)      -- synth, feedback beeps
audio.play(path)             -- WAV/PCM from the SD card
audio.mic_level() -> 0..1    -- RMS, for visualisers
```

Built on the BSP's audio entry points. No MP3 decoding, no resampling.

### Phase 4 — Networking (~5 days)

```lua
net.scan()    -> {{ssid, rssi}, ...}
net.connect(ssid, pass)
net.status()  -> "connected" | "connecting" | "disconnected"
net.get(url, opts)  -> body, status
net.post(url, opts) -> body, status    -- HTTPS, so LLM APIs work
```

Credentials come from **both** `/wifi.conf` on the SD card **and** an on-screen picker
with the touch keyboard. `lvgl.textarea` and `lvgl.keyboard` already exist in the
bindings, so the picker is wiring rather than new widgets — and text entry becomes an SDK
capability apps can use too.

LLM pass-throughs are a nice-to-have riding on `net.post` + HTTPS, not a separate feature.

If this phase slips, everyone still has a working device and four app categories.

## Out of scope

- **Tailscale** — dropped, see Context
- **Emscripten simulator** — pointless with one board per person
- **Camera, BLE, Wi-Fi app upload** — not asked for; Wi-Fi upload could follow Phase 4
- **USB Mass Storage** — the S3's serial/JTAG console and TinyUSB cannot both own the USB
  pins, so enabling MSC costs the serial console. Bad trade during a hackathon.

## Verification

- **Phase 1**: a second person clones the repo, flashes with `flash.sh` on a machine with
  no ESP-IDF, pushes an app with `push.py`, and runs it. This is the real test.
- **Phase 2–4**: one example app per capability, living in `apps/` as documentation that
  cannot rot — a level or dice app for the IMU, a clock for the RTC, a soundboard for
  audio, a weather or LLM app for `net`.
- **Regression**: launch and exit apps repeatedly, confirming PSRAM returns to the same
  free figure. This already holds and must keep holding.
- **Robustness**: an app that errors, and an app that loops forever, must both leave the
  launcher usable via PWR.

## Risks

1. **Networking slips.** Contained by being last; four app categories survive without it.
2. **API churn after people start writing.** Mitigated by the freeze, and by phases 2–4
   only adding new modules rather than changing existing ones.
3. **Serial push conflicts with logging.** Needs framing designed up front, not bolted on.
4. **The launcher maintainer is a bottleneck for firmware updates.** Acceptable at this
   size; CI is the escape hatch if it bites.
