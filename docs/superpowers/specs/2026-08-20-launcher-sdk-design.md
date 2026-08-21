# Launcher SDK — Design v2

**Date:** 2026-08-20
**Status:** Draft for review — revised after a four-persona review (embedded, app author, security, QA/delivery)

## What changed in v2, and why

v1 was reviewed by four personas. It contained one claim that was false, two bugs in
shipped code, and a hole big enough to sink most of the app categories. Summary of the
substantive corrections, because they justify most of this document:

| Finding | v1 said | Reality |
| --- | --- | --- |
| **No timer API exists** | Phase 2 "unlocks clocks, pedometers, workout counters" | Apps run code *only* on touch. Zero `lv_timer_create` bindings. Clocks, stopwatches, animation, live sensor display were all impossible, and the Phase-2 API freeze would have cemented that. |
| **App exit raced the LVGL task** | "verified, no leak" | `display_service_close()` deleted the app's LVGL tree outside the lock while the LVGL task ran on the other core. **Fixed and verified.** |
| **A Lua typo froze the display forever** | "errors are caught, not fatal" | Bindings `longjmp` past their unlock; `lvgl_mux` is `portMAX_DELAY`. **Fixed and verified.** |
| **Serial push has no input path** | "listens on the existing USB serial" | Console is UART-primary with USB *secondary*, which is output-only. Needs a console reconfiguration. |
| **Memory gate measured the wrong thing** | "record free internal DRAM at boot" | Boot is when nothing is allocated. Misses the 32 KB app stack, the 64 KB LVGL pool, and contiguity. |

Three reviewers also reported that `docs/APP_CONTRACT.md` describes an obsolete `app.*` API.
**This is false** — it was rewritten in `b80a466`; they were served a stale cached copy.
Recorded here so nobody re-opens it.

## Context

The launcher works end to end: apps are Lua files on SD, the launcher lists them, tapping
one runs it in its own VM, PWR returns to the list, and PSRAM returns to an identical free
figure across cycles.

### Fixed constraints

| | |
| --- | --- |
| Participants | 5–6, mixed embedded experience |
| Boards | **One each** — no sharing, **no spares** |
| Runway | ~2 weeks = **10 working days** |
| Board | Waveshare ESP32-S3-Touch-AMOLED-1.8 (V2), 368×448, 8 MB PSRAM, 16 MB flash |

One board each means app authors never need ESP-IDF, and kills the Emscripten simulator.
It also means **a dead board removes a participant** — buy 1–2 spares, it is the cheapest
risk mitigation available.

### Wanted app categories

Motion toys · clocks and watch faces · pedometers and workout counters · games and demos ·
sound · BLE (advertising, GATT server, HID) · battery utilities · network apps ·
LLM pass-throughs. Tailscale was raised and dropped.

## Architecture

### Capability modules

One Lua module per capability, an ESP-IDF component under `launcher/components/`,
registered through `cap_lua` and reached with `require`.

| Capability | Source | Cost |
| --- | --- | --- |
| `store`, `json`, `system` | Vendor from esp-claw, zero dependencies | Cheap |
| `timer` | **Launcher-side, no vendored changes** (see below) | Cheap |
| `imu` | `waveshare/qmi8658` v2.0.1 + hand-written binding | Medium |
| `rtc`, `battery` | No component; write I²C drivers for PCF85063 / AXP2101 | Medium |
| `audio` | BSP provides `bsp_audio_init` + speaker/mic codec init | Medium |
| `ble` | Vendor `lua_module_ble` + `_hid` (NimBLE) | Medium-large |
| `net` | Nothing reusable; `esp_wifi` + `esp_http_client` + TLS | Large |

Traps found while scoping: esp-claw's `lua_module_imu` is the **wrong chip** (BMI270 /
ICM42670, not QMI8658), and its `lua_module_audio` drags in MP3 decode and resampling we
do not need. Also note vendoring is never free — `lua_module_lvgl` required hand-writing
`cap_lua` (83 lines) *and* `display_service` (175 lines). Budget a shim for BLE too.

### The timer — the single most important addition in v2

Apps currently run code only when a finger touches a widget. That makes clocks,
stopwatches, animation, games and live sensor display impossible.

The fix needs **no changes to vendored code**: the launcher's pump loop already runs
periodically on the app task, which is exactly where Lua callbacks must run.

```lua
timer.every(1000, function() ... end)   -- returns a handle
timer.after(250,  function() ... end)
handle:cancel()                          -- all timers auto-cancel on app exit
```

The pump keeps a small list of `{deadline, period, lua_ref}`, fires whatever is due, and
shortens its own wait to the next deadline. Ship a plain `update(dt)` global first if
`timer` slips — ten lines, and it unblocks the same categories.

**This lands in Phase 1.** Without it the SDK is a static-poster generator, and freezing
the API at end of Phase 2 would make it permanent.

### Four rules every capability module must follow

**1. Register cleanup at boot, not per launch.** `cap_lua`'s cleanup table is a global
8-slot array. Registering from `luaopen_*` — the obvious reading — runs *per app launch*
and silently overflows on the second launch with seven modules. Register from your
module's one-time `*_register()` function instead. Cleanups run for every app regardless
of use, so each must be **no-op-safe and idempotent**. Raise `MAX_CLEANUPS`/`MAX_MODULES`
and check the return value at boot.

**2. Degrade, don't raise.** Return `nil, "message"`. Missing SD, absent Wi-Fi, or an
unavailable sensor must not kill the app.

**3. Own your task, drain from the pump.** *(Reworded — v1 had this backwards.)* LVGL is
**not** driven by the pump; `esp_lvgl_port` runs its own task and the pump drains only the
queued Lua callbacks. NimBLE likewise creates its own host task and cannot be pumped. So
the rule is the pattern LVGL already demonstrates: **your task enqueues, the app pump
dequeues and calls into Lua.** Never enter a `lua_State` from a foreign task.

**4. Blocking C functions must poll the stop flag and have a timeout.** `net.get`,
`net.connect`, `audio.play` and TLS handshakes can block for seconds. A Lua debug hook
does not fire inside C, so without this the PWR escape hatch stops working exactly as the
SDK grows. See below.

### Stopping a runaway app

PWR sets an atomic flag read in the pump loop; a Lua `while true do end` never reaches the
pump. Fix: `lua_sethook(L, hook, LUA_MASKCOUNT, 10000)`, installed **on the app task
before `luaL_dofile`** — never cross-task, which would race `L->hook` and walk the
CallInfo chain under the app.

Four holes to close, all identified in review:

- **`debug.sethook(nil)` disables it in one line.** Nil out `debug` in the app VM.
- **`pcall` swallows the error.** The hook must *latch*: on first fire, re-arm with count 1
  so every subsequent instruction re-raises and forward progress is impossible.
- **Coroutines start with `hookmask == 0`.** Either nil out `coroutine` or set the hook on
  each new thread.
- **Hooks never fire inside C functions** — hence rule 4.

Hard-deadline fallback is **`esp_restart()`, not `vTaskDelete`**. A deleted task may still
hold `lvgl_mux`, the I2C mutex or a FATFS lock, which produces a frozen display — the exact
failure this is meant to prevent.

Current behaviour, stated accurately: `CONFIG_ESP_TASK_WDT_PANIC` is unset, so a spinner
produces a watchdog warning every 5 s **forever**, flooding the console — which will also
corrupt serial push, since they share it.

### Not a sandbox — state it plainly

`luaL_openlibs` gives apps the full stdlib: `io`, `os`, `debug`, `package`, `coroutine`.
An app can read or overwrite anything on the SD card, including other people's apps and
any credentials, and `os.exit()` restarts the chip.

**Building a sandbox is the wrong call** — days of work, breaks legitimate apps, and
everyone can already reflash everyone's board. The right move is to delete the false
promise from the app contract and replace it with the honest trust statement: *apps run
unsandboxed; only run apps you'd trust with your board.*

Do nil out the four things that break the launcher rather than the user: `debug`,
`os.exit`, `os.execute`, `package`. Also drop `CONFIG_LUA_MAXSTACK` from 1000000 — runaway
recursion currently tries to allocate ~16 MB of Lua stack before erroring.

### Launcher owns `lvgl.init()`

Apps must currently call it or exit instantly with a confusing error naming a function
they never called. The launcher will call it on the **app's own `lua_State`** after
opening modules, so apps begin at `local scr = lvgl.screen()`.

Idempotency is narrower than v1 implied: `runtime_owner == L` → return true;
`runtime_owner != L` → error. A second call carrying options (`font_path`, `font_size`)
must warn rather than silently ignore them.

### Memory: measure the right thing

Baseline is **173,631 bytes** free internal DRAM after boot. That number alone is
misleading, because internal DRAM — not PSRAM — is what runs out. Three consumers v1
missed:

- **32 KB app task stack per running app**, internal, not in the baseline.
  **Free win:** `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y` is already set, so
  `xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)` recovers all of it.
- **A fixed 64 KB LVGL pool** (`CONFIG_LV_MEM_SIZE_KILOBYTES=64`, builtin TLSF) shared by
  the launcher and every app and never reset. Add `lv_mem_monitor` to the memory check.
- **Wi-Fi/lwIP buffers and the NimBLE controller pools**, which are the real pressure.
  Two knobs absent from v1: `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` and
  `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`.

Conversely v1 **over-worried TLS**: `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` already
pushes mbedTLS's 16 KB buffers to PSRAM, and they are tunable.

Track `heap_caps_get_largest_free_block()` and `heap_caps_get_minimum_free_size()` **at
peak with an app running**, not free-total at boot.

## Phases

Estimates are person-days.

### Phase 0 — Two hours, day 1, before anything else

1. **Push to a git remote.** Everything lives on one laptop. Five minutes, blocks five people.
2. **`.gitignore`**: `wifi.conf`, `secrets*`, `*.key`, `.env` — before there is a remote or
   anything to leak.
3. **The memory spike.** Enable BT + Wi-Fi, bring both radios up, log largest free block.
   This either de-risks Phases 3–4 or tells you to re-scope them **while re-scoping is
   still cheap**. Deferring it puts the discovery at day 7 of 10.
4. **Order spare boards.**

### Phase 1 — Unblock five people, and make the launcher genuinely robust (~5 days)

Revised up from 3. It absorbs the timer, the console rework, and the robustness items,
and it does **not** parallelise.

- **`timer.every` / `timer.after`** — the SDK is not usable without it
- **Runaway-app protection** — hook + latching + `debug` removed + `esp_restart` fallback
- **Stdlib trim** (`debug`, `os.exit`, `os.execute`, `package`) and `LUA_MAXSTACK`
- **`firmware/` + `flash.sh`**, pip-installing `esptool`. Put the BOOT/PWR recovery
  sequence in the script's error message — the first reflash over a hung app will need it.
- **Serial push + `push.py`**, including the console reconfiguration
- **Refresh button** — must clear the `s_mounted` latch and remount, rebuild the row UI
  rather than reusing pointers into the rescanned array, and be disabled while an app runs
- **Scrolling app list** — verify first, it may already work
- **App errors shown on screen** — including callback errors, which are currently silent
- **`luaL_traceback` message handler** — five lines, and the contract already (wrongly)
  promises tracebacks
- **Reconcile the app contract** with reality: no-sandbox statement, document `lvgl.font_load`,
  state that Lua 5.5 stdlib is available, fix the callback-argument and VM-cost claims

Serial push design items: framing **with base64 or hex** (the console VFS translates
CR/LF), **log suppression during transfer** (`esp_log_set_vprintf`) not just framing,
**filename validation** (basename only, `.lua`, size cap), and port contention with
`idf.py monitor`. Scope it to `.lua` — WAVs and fonts need a different path. Note it is
*not* a recovery path: a crashed app takes USB with it.

### Phase 2 — Cheap wins (~3 days)

Revised down; `store`/`json`/`system` are vendored and `imu` has a driver.

```lua
store.get(k) / store.set(k, v)
imu.accel() / imu.gyro() / imu.steps()
rtc.now() / rtc.set(t)
battery.percent() / .volts() / .charging()
json.encode(t) / json.decode(s)
```

**`rtc.set()` is not optional.** A PCF85063 that has never been set returns garbage, and
the only other way to set it is NTP — in Phase 4, the droppable phase. Ship a set-time
screen using the existing `roller`/`spinbox` widgets.

**`store` must not live in NVS.** The NVS partition is 24 KB and is shared with Wi-Fi
credentials and, after Phase 3, BLE bonding keys; one app writing in a timer callback
would break the radios in a way that looks nothing like the cause. Use the unused 4 MB FAT
`storage` partition or the SD card, and write atomically (`tmp` + rename) — power loss
mid-write on FAT can corrupt the whole card.

Verify the `waveshare/qmi8658` driver actually exposes the hardware pedometer before
promising step counting. Fifteen minutes.

**API freezes at the end of this phase**, on a named date.

### Phase 3 — Audio and BLE (~4.5 days)

Audio (~1.5 d): `audio.tone`, `audio.play` (WAV from SD), `audio.mic_level`. Built on BSP
entry points; no MP3 decode, no resampling.

BLE (~3 d, revised up): vendored NimBLE peripheral + HID.

```lua
ble.init() / ble.set_name(s) / ble.adv_start{...} / ble.gatts_define(profile)
```

**Peripheral only** — no Central, so no heart-rate straps. Confirmed acceptable.
Keep NimBLE (~40–60 KB) over Bluedroid (100 KB+).

Two things that turn 2 days into 4 if discovered late:
- **GATT reads cannot be deferred to the pump.** `ble_gatts_access_fn` must fill the
  response synchronously on the host task, and you cannot enter the app's `lua_State`
  there. Reads must be served from a C-side cached value that Lua writes. Check what
  esp-claw actually does here **before** committing to the estimate.
- **HID needs bonding to function**, not merely to be secure — macOS/Windows/iOS reject
  HID from unpaired peripherals. Persisting keys loops back to the NVS budget.

Default the advertised name to include a MAC suffix; six boards advertising the same name
in one room is its own afternoon.

### Phase 4 — Networking (~5 days)

```lua
net.scan() / net.connect(ssid, pass) / net.status()
net.get(url, opts) / net.post(url, opts)
```

Credentials from `/wifi.conf` **and** an on-screen keyboard picker (widgets already exist).

- **Use `esp_crt_bundle_attach`. Never `CONFIG_ESP_TLS_INSECURE`.**
- **SNTP is required, not optional.** Certificate validity needs a correct clock; a cold
  board reads every cert as not-yet-valid, and the "fix" people find at 2am is disabling
  verification. This is the mechanism by which the API key ends up in cleartext.
- **API keys live outside `apps/`** — e.g. `/sdcard/secrets.conf`, never in a `.lua` file
  in the shared repo. Own key per person, spend cap, revoked after.

## Effort and sequencing

**17.5 person-days against 10 working days**, with Phase 1 (5 days) serial. Critical path
is 5 + (12.5 / 2) = **11.25 days for two people. It does not fit.**

That is the honest arithmetic, and v1's "only achievable because two people" hand-waved
it. The plan is therefore a **priority queue with a freeze date**, not a schedule:

- Phase 0 and Phase 1 are non-negotiable.
- Phase 2 is what makes the SDK worth having.
- Phase 3 and 4 are what fits.

Also not in any estimate, and real: five example apps (which *are* the verification, 3+
days), supporting five teammates, contract updates every phase, integration, and the
crash-recovery tax — which is a multi-minute physical ritual per crash, in the two phases
most likely to crash.

**Put dates on:** API freeze (end of Phase 2), firmware freeze (T-2), and a dry run at T-1
where everyone flashes and runs everyone else's apps.

## Out of scope

Tailscale · Emscripten simulator · BLE Central/GATT Client · camera · USB Mass Storage
(shares the USB PHY with the console) · a Lua sandbox (see above) · app icons · app store UI.

Worth noting: 8 MB of flash is unallocated and there is no OTA partition. Two 4 MB OTA
slots plus `net`-based OTA would beat CI as the answer to the maintainer bottleneck, if
Phase 4 lands.

## Verification

- **Phase 0:** the spike produces a number, and that number re-scopes Phases 3–4 or doesn't.
- **Phase 1:** a second person, **on their own board**, on a machine with **no ESP-IDF**,
  clones, runs `flash.sh`, pushes an app with `push.py`, and runs it. Board diversity
  matters — only one board has ever run this firmware.
- **Runaway app:** test all three variants — bare `while true do end`, one wrapped in
  `pcall`, and one inside a coroutine. v1 tested only the easiest.
- **Phases 2–4:** one example app per capability, in `apps/`, as documentation that cannot rot.
- **Unhappy path:** a `smoke.lua` that calls every SDK function with the SD card ejected
  and radios down, asserting nothing throws. This is the only check for Rule 2, and there
  is currently none.
- **Rule 3:** `grep -rn "xTaskCreate" launcher/components/` as a review gate.
- **Hardware release:** launch → exit → launch again, per peripheral. Catches a missing
  cleanup callback.
- **Memory:** largest free block and minimum free, **at peak with an app running**, plus
  `lv_mem_monitor`. Run the launch/exit loop 50 times and watch internal, not just PSRAM.
- **CI:** a compile-only job in `espressif/idf:v5.5.5`. The value is catching build breakage
  between two people editing shared CMake and `sdkconfig` files — not firmware distribution.

## Risks

1. **Internal DRAM exhaustion.** **Measured, not assumed** — Phase 0's spike (2026-08-20)
   brought up NimBLE and Wi-Fi STA together and logged `heap_caps_get_largest_free_block()`
   at each stage:

   ```
   SPIKE baseline       internal_free=139711 largest=61440 min_ever=63244
   SPIKE after_nvs      internal_free=135627 largest=61440 min_ever=62540
   SPIKE after_nimble   internal_free=101547 largest=31744 min_ever=62252
   SPIKE after_wifi     internal_free=24795  largest=16384 min_ever=24728
   ```

   `largest` after Wi-Fi comes up is **16 KB** — under the 20 KB floor. **Decision: re-scope
   now.** BLE and Wi-Fi become mutually exclusive.

   **How exclusivity is enforced: lazy init, not a manifest.** Apps are single `.lua` files
   with no metadata, so there is nothing to declare in. Instead the launcher brings a radio
   up the *first time* an app touches `net.*` or `ble.*`, and tears it down when the app
   exits. The second radio then returns `nil, "radio in use by net"` per rule 2. This needs
   no new file format, no manifest parser, and it cannot get out of sync with what the app
   actually does.

   **The static cost is unavoidable and already paid.** Merely enabling both stacks in
   `sdkconfig` drops the baseline from 173,631 to 139,711 — about 34 KB — before either is
   initialised. Since apps are shared and any app may want either radio, both must be
   compiled in. Budget from 139,711, not 173,631.

   Remaining headroom with exactly one radio up: **BLE alone leaves 101,547 free / 31,744
   largest**, which is workable. Wi-Fi alone was not measured in isolation (the spike
   brought it up on top of NimBLE) — measure it before Phase 4 commits to TLS buffer sizes.
2. **The plan does not fit the runway.** Managed by the priority queue and freeze date, not
   by optimism.
3. **API churn.** The timer arriving in Phase 1 rather than after the freeze is the whole
   point of v2.
4. **A module forgets its exit cleanup**, or registers it per-launch and overflows the
   8-slot table. Covered by rule 1 and the launch-exit-launch check.
5. **Single points of failure:** no remote (Phase 0), no spare boards (Phase 0), one person
   holding the crash-recovery knowledge (move it into the README, not just CLAUDE.md), and
   five boards that have never run this firmware.
