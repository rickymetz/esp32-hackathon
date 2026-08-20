# Launcher SDK — Design

**Date:** 2026-08-20
**Status:** Draft for review

## Context

The shared app launcher works end to end: apps are Lua files on the SD card, the launcher
lists them, tapping one runs it in its own VM, and PWR returns to the list. Verified on
hardware, including that PSRAM returns to an identical free figure across launch/exit
cycles.

Today it exposes display and touch only. This design covers the two weeks before the
hackathon, so 5–6 people can write apps against a stable SDK.

### Fixed constraints

| | |
| --- | --- |
| Participants | 5–6, mixed embedded experience |
| Boards | **One each** — no sharing |
| Runway | ~2 weeks, i.e. **10 working days** |
| Board | Waveshare ESP32-S3-Touch-AMOLED-1.8 (V2), 368×448, 8 MB PSRAM, 16 MB flash |
| Free internal DRAM after boot | **173,639 bytes** (measured) |

Two of these do more work than they look like they do:

**One board each** means app authors never need ESP-IDF — they flash a prebuilt binary
once, then only copy files. It also kills the Emscripten simulator, which was only ever
insurance against sharing a single board.

**Free internal DRAM** is the scarce resource in this project, not PSRAM. See Risk 1.

### Wanted app categories

Motion toys · clocks and watch faces · pedometers and workout counters · games and demos ·
sound · BLE (advertising, GATT server, HID) · battery utilities · network apps ·
LLM pass-throughs.

Tailscale was raised and **dropped**: no official ESP32 client, and a real one means
WireGuard plus the coordination protocol.

## Approach

Distribution first, then cheap capability wins, then audio and BLE, then networking.

Rejected: **network-first** (highest ceiling, but if it slips the cheap wins never land
and people are left with display-only apps) and **build-on-demand** (lowest waste, but
people hit walls mid-build and block on one person).

The chosen order front-loads what blocks five people, banks the high-value-per-hour APIs
early, and puts the riskiest work where slipping is survivable.

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
| `audio` | BSP already provides `bsp_audio_init` + speaker/mic codec init | Medium |
| `ble` | Vendor `lua_module_ble` + `_hid`; needs only `bt`, `esp_timer`, `nvs_flash`, `cap_lua` | Medium |
| `net` | Nothing reusable; `esp_wifi` + `esp_http_client` + TLS | Large |

Two traps found while scoping, both of which would have cost a day each:

- **esp-claw's `lua_module_imu` is the wrong chip.** It supports BMI270 and ICM42670, not
  the QMI8658 fitted here.
- **esp-claw's `lua_module_audio` is far heavier than we need.** It pulls
  `esp_audio_codec`, `esp_asrc` and `esp-dsp` for MP3 decode and resampling. The BSP path
  gives us tone, WAV and mic level without any of it.

### Three rules every capability module must follow

These are architectural requirements, not style preferences. Each exists because
violating it breaks the launcher for everyone.

**1. Release hardware on app exit.** Every module that claims a peripheral must register
a `cap_lua_register_exit_cleanup` callback that frees it. Today only the display session
does this. If the BLE or audio module skips it, the *second* app to use that peripheral
fails, and the failure looks like a bug in the app rather than the launcher.

**2. Degrade, don't raise.** Modules return `nil, "message"` on failure. A missing SD
card, absent Wi-Fi, or unavailable sensor must not kill the app.

**3. Pump cooperatively.** Any module with its own event queue (BLE, like LVGL) is driven
from the app task's single pump loop. No module gets its own polling task, and no module
may starve another.

### Launcher owns `lvgl.init()`

Apps currently must call `lvgl.init()` or fail confusingly. The launcher will call it
before running the app, so apps begin at `local scr = lvgl.screen()`.

This preserves the isolation property that matters: the display session still belongs to
the app's VM, so exiting deletes the app's screen and every widget parented to it.

Implementation note: the launcher calls it via `lua_getglobal` + `lua_pcall` after opening
modules. `lvgl.init()` must be made **idempotent** — it currently errors with "already
initialized", and existing apps (and every esp-claw example) call it explicitly. A second
call should return success, not fail.

### Stopping a runaway app

**The current design cannot stop an app that never yields.** PWR sets an atomic flag which
is checked in the pump loop; a Lua `while true do end` never reaches the pump, so the flag
is never read. The app contract says "do not block", but with 5–6 people someone will do
it by accident, and the failure mode is a device that needs a physical power cycle.

Fix: install a Lua debug hook with `lua_sethook(L, hook, LUA_MASKCOUNT, 10000)`. The hook
fires every ~10k VM instructions, checks the stop flag, and calls `lua_error` to unwind
the app. This is the standard technique for interrupting embedded Lua and costs a few
percent of throughput.

This belongs in **Phase 1**, not later — it is the difference between "the launcher is
robust" and "the launcher is robust as long as nobody makes a mistake".

### API freeze

The surface below freezes **at the end of Phase 2**, which is when app authors have enough
to start real work. Additions after that are additive only: new modules and new functions,
never changed signatures.

## Phases

Estimates are **person-days**. See Effort and sequencing for how they fit the runway.

### Phase 1 — Unblock five people, and make the launcher actually robust (~3 days)

Nothing else matters until this is done.

- **Git remote and repo hygiene** — there is currently no remote; nobody can clone
- **`firmware/` + `flash.sh`** that pip-installs `esptool` and flashes. App authors never
  install ESP-IDF; only the launcher maintainer does. No CI initially; add it if manual
  refreshes become annoying.
- **Runaway-app protection** via the debug hook above
- **Serial file push**: launcher listens on the existing USB serial; `push.py` sends a
  `.lua` straight to the SD card
- **Refresh button** — rescan apps without rebooting
- **Scrolling app list** — the current list breaks past roughly five apps
- **App errors shown on screen**, not only on serial

Serial push exists because the alternative is eject SD → adapter → Mac → copy → back →
reboot: roughly 30 seconds, repeated 20+ times per author per day. It is the worst
friction in the current design and it hits everyone.

Two things to design up front rather than discover:

- **Framing.** Push traffic shares the port with log output. Needs a delimiter and a
  checksum, not just raw bytes.
- **Port contention.** `idf.py monitor` holds the port, so pushing while monitoring will
  fail — the same class of problem as flashing while monitoring. `push.py` should detect
  this and say so plainly rather than hanging.

App sharing is the repo: apps live in `apps/`, people push theirs, others `git pull`.

### Phase 2 — Cheap wins (~4 days)

Unlocks motion toys, clocks, pedometers and workout counters.

```lua
store.get(k) / store.set(k, v)     -- survives reboot; namespaced, see below
imu.accel() -> x, y, z             -- g
imu.gyro()  -> x, y, z             -- deg/s
imu.steps() -> count               -- QMI8658 hardware pedometer
rtc.now()   -> {year, month, day, hour, min, sec, wday}
battery.percent() / .volts() / .charging()
json.encode(t) / json.decode(s)
```

`store` is namespaced by the app's **filename stem** (`weather_clock.lua` → namespace
`weather_clock`). Two consequences to document: renaming a file orphans its data, and two
people's apps sharing a filename share storage.

The QMI8658's **hardware pedometer** makes step counters and workout apps nearly free
rather than a signal-processing exercise.

**API freezes at the end of this phase.**

### Phase 3 — Audio and BLE (~3.5 days)

#### Audio (~1.5 days)

```lua
audio.tone(freq_hz, ms)      -- synth, feedback beeps
audio.play(path)             -- WAV/PCM from the SD card
audio.mic_level() -> 0..1    -- RMS, for visualisers
```

Built on the BSP's audio entry points. No MP3 decode, no resampling.

#### BLE (~2 days)

Vendored from esp-claw exactly as `lua_module_lvgl` was — it needs only IDF built-ins plus
the `cap_lua` shim already written.

```lua
ble.init() / ble.set_name(s)
ble.adv_start({ data = { name = "..." } })   -- advertise
ble.gatts_define(profile)                    -- GATT server: services, characteristics,
                                             -- read/write/notify/indicate
-- plus lua_module_ble_hid for keyboard, mouse, media keys
```

**Peripheral only.** The module explicitly does not support scanning, observer mode,
Central mode, or GATT Client. The board can advertise and be connected *to*, but cannot
connect *to* another device such as a heart-rate strap. Confirmed acceptable.

**It uses NimBLE, not Bluedroid** — roughly 40–60 KB of internal DRAM rather than 100 KB+.
Keep it that way; switching stacks would likely make BLE and Wi-Fi mutually exclusive.

### Phase 4 — Networking (~5 days)

```lua
net.scan()    -> {{ssid, rssi}, ...}
net.connect(ssid, pass)
net.status()  -> "connected" | "connecting" | "disconnected"
net.get(url, opts)  -> body, status
net.post(url, opts) -> body, status    -- HTTPS, so LLM APIs work
```

Credentials come from **both** `/wifi.conf` on the SD card **and** an on-screen picker
using the touch keyboard. `lvgl.textarea` and `lvgl.keyboard` already exist in the
bindings, so the picker is wiring rather than new widgets — and text entry becomes an SDK
capability apps can use too.

LLM pass-throughs ride on `net.post` + HTTPS; not a separate feature.

If this phase slips, everyone still has a working device and six app categories.

## Effort and sequencing

**15.5 person-days against 10 working days.** That only fits with two people on the
launcher, and only because the work parallelises cleanly: after Phase 1, each capability
is an independent component behind the `cap_lua` interface, so two people can build
different modules without touching the same files.

Phase 1 is the exception — it is mostly launcher-core work and does not parallelise well.
Treat it as one person, three days, and start it immediately.

**Each phase gets its own implementation plan.** Phase 1 is blocking and separable; plan
and ship it before planning the rest.

If the runway compresses, drop from the bottom: Phase 4 first, then BLE, then audio. Never
drop Phase 1 — without it there is no way for five people to participate at all.

## Out of scope

- **Tailscale** — dropped, see Context
- **Emscripten simulator** — pointless with one board per person
- **BLE Central / GATT Client** — connecting *to* sensors; not vendorable
- **Camera; Wi-Fi app upload** — the latter could follow Phase 4 and would supersede
  serial push
- **USB Mass Storage** — the S3's serial/JTAG console and TinyUSB cannot both own the USB
  pins, so MSC costs the serial console. Bad trade during a hackathon.
- **App icons, multitasking, app store UI** — not asked for

## Verification

Every phase ends with a check that would fail if the phase were faked.

- **Phase 1:** a second person, on a machine with **no ESP-IDF**, clones the repo, runs
  `flash.sh`, pushes an app with `push.py`, and runs it. Separately: an app containing
  `while true do end` must still be killable with PWR.
- **Phases 2–4:** one example app per capability, living in `apps/` as documentation that
  cannot rot — a level or dice app for the IMU, a clock for the RTC, a soundboard for
  audio, an HID remote for BLE, a weather or LLM app for `net`.
- **Memory, every phase:** record free internal DRAM at boot and compare against the
  173,639-byte baseline. A phase that costs more than its budget (Risk 1) stops and gets
  re-scoped rather than continuing.
- **Regression, every phase:** launch and exit apps repeatedly; PSRAM must return to the
  same free figure. This holds today and must keep holding.
- **Hardware release:** launch an app that uses a peripheral, exit, launch it again. The
  second run must work. This is the check that catches a missing exit-cleanup callback.

## Risks

**1. Internal DRAM exhaustion — the top technical risk.** 173,639 bytes free today.
NimBLE needs ~40–60 KB and Wi-Fi ~50 KB, so both together plausibly consume 60% of what
is left, before TLS buffers, which are also internal and can be tens of KB per connection.
BLE and Wi-Fi being simultaneously usable is **an assumption, not a fact**. Measure after
Phase 3 and again during Phase 4. If they do not fit, the fallback is making them mutually
exclusive — an app declares which radio it wants — rather than shipping something that
fails unpredictably under memory pressure.

**2. Networking slips.** Contained by being last; six app categories survive without it.

**3. API churn after people start writing.** Mitigated by the end-of-Phase-2 freeze and by
later phases only adding modules rather than changing them.

**4. A capability module forgets its exit cleanup**, so the second app to use that
peripheral fails and it looks like an app bug. Mitigated by the explicit verification step
above.

**5. The launcher maintainer is a bottleneck for firmware updates.** Acceptable at this
size; CI is the escape hatch.
