# Phase 2 — Capability Modules Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Give apps persistent storage, motion, time, battery and JSON — the capabilities that unlock motion toys, clocks, pedometers and workout counters.

**Architecture:** One Lua module per capability, each its own ESP-IDF component under `launcher/components/`, registered through `cap_lua` and reached with `require`. Phase 1 established the pattern: `app_timer.c` is the reference implementation to copy.

**Tech Stack:** ESP-IDF v5.5.5 · LVGL 9.5 · Lua 5.5 · Waveshare BSP ^2.0.3 · `waveshare/qmi8658` ^2.0.1

## Prerequisite

**Phase 1's hardware verification must complete first.** Tasks 8–11 of
`2026-08-20-phase0-phase1.md` are code-complete but unverified — the board wedged mid-run.
Recover it, run those checks, and fix any fallout **before** starting Task 1 here. Building
on unverified foundations is how a hackathon loses a day.

## Global Constraints

Learned the hard way in Phase 1. Every one of these cost real time.

- **`LUA_32BITS=1` is set build-wide** in `launcher/CMakeLists.txt`. Never add a per-component
  copy. Getting this wrong aborts in `luaL_checkversion_` with no useful message.
- **Register exit cleanups from your one-time `*_register()`**, never from `luaopen_*` —
  the latter runs per app launch and silently overflows an 8-slot global table.
- **Never hand out a pointer into shared state.** Return copies. `app_registry_get()` was
  deleted for exactly this reason after it caused a NULL-deref crash.
- **Lock order is `s_app_mutex` (outer) → `bsp_display_lock` → registry/module lock (inner).**
  Never hold a module lock across a callback into `launcher_main.c`.
- **Modules degrade, never raise:** return `nil, "message"`. Absent hardware must not kill an app.
- **Any blocking C function must poll `cap_lua_runtime_stop_requested()` and have a timeout.**
  The Lua interrupt hook does not fire inside C calls, so a blocking call without this
  silently breaks PWR-to-exit.
- **New I²C devices attach via `i2c_master_bus_add_device` on the BSP's existing bus handle.**
  A second bus handle on GPIO14/15 produces intermittent touch failures that look like hardware faults.
- **Touch targets ≥ ~200×100 px.** Smaller ones drop roughly half their taps.
- ESP-IDF v5.5.5; `. ~/esp/esp-idf/export.sh > /dev/null` in every shell.
- Board `/dev/cu.usbmodem101`. If the port seems to vanish, **retry** — that was a false
  alarm three separate times in Phase 1.
- **Never flash firmware that can hang at boot.** A crash takes native USB down and needs
  physical BOOT recovery, which is unavailable when nobody is at the desk.

**Standard loop** (no SD card shuffling, no screen taps):

```bash
cd launcher && . ~/esp/esp-idf/export.sh > /dev/null && idf.py build && \
  ../.venv/bin/esptool --chip esp32s3 --port /dev/cu.usbmodem101 -b 460800 \
  --before default_reset --after hard_reset write_flash --flash_mode dio \
  --flash_size 16MB --flash_freq 80m \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin 0x10000 build/launcher.bin
cd .. && ./.venv/bin/python tools/push.py apps/NAME.lua
# then RUN NAME.lua / STOP over serial; read with tools/read_serial.py
```

---

## File Structure

| Path | Responsibility |
| --- | --- |
| `launcher/components/lua_store/` | `store` — persistent key/value, namespaced per app |
| `launcher/components/lua_json/` | `json` — encode/decode |
| `launcher/components/lua_imu/` | `imu` — QMI8658 accel, gyro, steps |
| `launcher/components/lua_rtc/` | `rtc` — PCF85063 read **and set** |
| `launcher/components/lua_battery/` | `battery` — AXP2101 percent, volts, charging |
| `apps/` | One example app per capability — documentation that cannot rot |

Each module mirrors `launcher/main/app_timer.c`: a `*_register()` called once from
`app_main`, a static `luaopen_*`, and an exit-cleanup callback that releases hardware.

---

### Task 1: `store` — persistent key/value

**Do this first.** Every other capability's example app wants to remember something, and
it has no hardware dependency to debug alongside the binding.

**Files:** create `launcher/components/lua_store/{lua_store.c,lua_store.h,CMakeLists.txt}`;
modify `launcher/main/launcher_main.c` (register), `launcher/main/CMakeLists.txt`;
create `apps/store_test.lua`.

**Interfaces produced:**
- `esp_err_t lua_store_register(void)` — call once at boot
- `void lua_store_set_namespace(const char *app_basename)` — called by the launcher per app
- Lua: `store.set(k, v) -> true | nil, err` · `store.get(k) -> value | nil` · `store.delete(k)` · `store.keys() -> {…}`

**Backing store: the FAT `storage` partition, NOT NVS.** `partitions.csv` gives `nvs` only
0x6000 (24 KB), shared with Wi-Fi credentials and — after Phase 3 — BLE bonding keys. One
app writing in a timer callback would exhaust it and break the radios in a way that looks
nothing like the cause. The unused 4 MB `storage` partition exists for this.

**Durability:** write to `<key>.tmp`, then `rename()`. A power cut mid-write on FAT can
otherwise corrupt the allocation table and take out every app on the volume.

**Namespacing:** by the app's filename stem, supplied by the launcher at launch. Document
that renaming a file orphans its data, and that two people's apps sharing a filename share
storage. It prevents accidents, it is not an isolation boundary.

- [ ] **Step 1: Write the failing test app** — `apps/store_test.lua` that increments a
      counter across runs, prints `STORE count=N`, and exercises `set`/`get`/`delete`/`keys`.
- [ ] **Step 2: Push and RUN it; confirm it fails** with `module 'store' not found`.
- [ ] **Step 3: Implement the component**, mirroring `app_timer.c`'s structure.
- [ ] **Step 4: Mount the storage partition** at boot (`esp_vfs_fat_spiflash_mount_rw_wl`)
      and wire `lua_store_register()` into `app_main`; call `lua_store_set_namespace()` in
      `lua_app_task` before `luaL_dofile`.
- [ ] **Step 5: Build, flash, RUN twice** — the count must persist across the two runs and
      across a reboot. Show all three numbers.
- [ ] **Step 6: Verify degradation** — behaviour when the key is absent, and when the value
      is a type that is not string/number/boolean. Must return `nil, msg`, never raise.
- [ ] **Step 7: Commit and push.**

---

### Task 2: `json` — encode and decode

Smallest task; vendor from `espressif/esp-claw`'s `lua_module_json` (zero dependencies).

**Files:** create `launcher/components/lua_json/`; modify the two CMakeLists and `launcher_main.c`;
create `apps/json_test.lua`.

**Interfaces produced:** `esp_err_t lua_json_register(void)`; Lua `json.encode(t) -> string`,
`json.decode(s) -> table | nil, err`.

- [ ] **Step 1:** Write `apps/json_test.lua` doing a round-trip of a nested table with
      strings, numbers, booleans, an array and a nil, printing the result.
- [ ] **Step 2:** Push and RUN; confirm `module 'json' not found`.
- [ ] **Step 3:** Vendor the component. Check its `idf_component.yml` for dependencies before
      copying — esp-claw's IMU module turned out to be for the wrong chip, so verify rather than assume.
- [ ] **Step 4:** Register it and build.
- [ ] **Step 5:** Flash, push, RUN — round-trip must be lossless. Confirm malformed input
      returns `nil, err` rather than raising.
- [ ] **Step 6: Commit and push.**

---

### Task 3: `imu` — QMI8658 motion

Unlocks motion toys, pedometers and workout counters — the largest single category.

**Files:** create `launcher/components/lua_imu/`; modify the two CMakeLists and `launcher_main.c`;
create `apps/level.lua` and `apps/steps.lua`.

**Interfaces produced:** `esp_err_t lua_imu_register(void)`; Lua `imu.accel() -> x, y, z` (g),
`imu.gyro() -> x, y, z` (deg/s), `imu.temp() -> celsius`, and `imu.steps() -> count` **if**
the driver exposes the hardware pedometer.

**Use the `waveshare/qmi8658` component (^2.0.1) from the ESP Component Registry.**
esp-claw's `lua_module_imu` supports BMI270 and ICM42670 — **not** the QMI8658 on this
board. Attach on the BSP's existing I²C bus handle.

- [ ] **Step 1: Settle the pedometer question first — fifteen minutes.** Read the
      `waveshare/qmi8658` headers and confirm whether the hardware step counter is exposed.
      The whole "step counters are nearly free" claim rests on this. If it is not exposed,
      say so, drop `imu.steps()` from this task, and note that step counting becomes a
      signal-processing job for a later phase.
- [ ] **Step 2:** Write `apps/level.lua` — a bubble level using `imu.accel()` on a
      `timer.every(50, …)`, printing `TILT x y` so it can be verified over serial without
      looking at the screen.
- [ ] **Step 3:** Push and RUN; confirm `module 'imu' not found`.
- [ ] **Step 4:** Add the dependency, implement the binding, register it.
- [ ] **Step 5:** Flash and RUN `level.lua`. **Verify the values are real, not stubs:** at
      rest one axis must read approximately ±1 g and the others near 0. State which axis.
- [ ] **Step 6:** If step counting exists, write `apps/steps.lua` and confirm the count
      changes when the board is moved — this one needs a human, so mark it clearly and
      defer it if nobody is present.
- [ ] **Step 7: Verify degradation** — unplug nothing, but confirm the code path when the
      sensor NAKs returns `nil, msg`.
- [ ] **Step 8: Commit and push.**

---

### Task 4: `rtc` — PCF85063 wall clock, read **and set**

**Files:** create `launcher/components/lua_rtc/`; modify the two CMakeLists and `launcher_main.c`;
create `apps/clock.lua`.

**Interfaces produced:** `esp_err_t lua_rtc_register(void)`; Lua
`rtc.now() -> {year, month, day, hour, min, sec, wday}` and `rtc.set(t) -> true | nil, err`.

**`rtc.set()` is not optional.** A PCF85063 that has never been set returns garbage, and the
only other way to set it is NTP — which lives in Phase 4, the phase most likely to be cut.
Without `set`, every clock app is wrong all day.

No driver component exists; write the I²C register access directly (BCD encoding, register
0x04 onwards). Attach on the BSP's bus handle.

- [ ] **Step 1:** Write `apps/clock.lua` — displays the time, updating on `timer.every(1000, …)`,
      and prints `CLOCK hh:mm:ss` so it is verifiable over serial.
- [ ] **Step 2:** Push and RUN; confirm `module 'rtc' not found`.
- [ ] **Step 3:** Implement the driver and binding.
- [ ] **Step 4:** Flash. Call `rtc.set()` with a known time, then read it back — confirm it
      round-trips, including the BCD boundaries (e.g. minute 09 → 10, hour 23 → 00).
- [ ] **Step 5:** Confirm the time **advances**: read, wait 3 s, read again.
- [ ] **Step 6:** Confirm it **survives a reboot** — set it, reflash, read it back.
      This is the check that proves the RTC is actually keeping time rather than a variable.
- [ ] **Step 7:** Add a set-time screen to the launcher using the existing `roller`/`spinbox`
      widgets, so a user can set the clock without Wi-Fi. Remember the ≥200×100 touch rule.
- [ ] **Step 8: Commit and push.**

---

### Task 5: `battery` — AXP2101 power

**Files:** create `launcher/components/lua_battery/`; modify the two CMakeLists and `launcher_main.c`;
create `apps/battery.lua`.

**Interfaces produced:** `esp_err_t lua_battery_register(void)`; Lua `battery.percent() -> 0..100`,
`battery.volts() -> number`, `battery.charging() -> boolean`.

The AXP2101 is at I²C `0x34`. No driver component; write the register access directly.
Note the vendor documents that its percentage is voltage-derived and fluctuates under load —
document that in the app contract rather than pretending it is precise.

- [ ] **Step 1:** Write `apps/battery.lua` printing `BATT pct=N v=N.NN charging=BOOL` on a timer.
- [ ] **Step 2:** Push and RUN; confirm `module 'battery' not found`.
- [ ] **Step 3:** Implement and register.
- [ ] **Step 4:** Flash and RUN. **Sanity-check the values are real:** volts should be
      roughly 3.7–4.2 for a lithium cell, or report the USB-powered reading. A percentage
      that is exactly 0 or exactly 100 every time is a stub, not a reading.
- [ ] **Step 5:** Confirm `charging()` flips when USB power is present versus absent — needs
      a human to unplug, so mark it and defer if nobody is present.
- [ ] **Step 6: Commit and push.**

---

### Task 6: Freeze the API and update the contract

**This is the deliverable that unblocks five people**, not an afterthought.

**Files:** modify `docs/APP_CONTRACT.md`, `CLAUDE.md`, `README.md`.

- [ ] **Step 1:** Document every new module with a worked example taken from an app that
      actually runs — not invented snippets.
- [ ] **Step 2:** Record the limits honestly: `store` namespacing caveats and its durability
      guarantee; the IMU's real axis orientation as measured; the RTC needing to be set once;
      the battery percentage being voltage-derived and noisy.
- [ ] **Step 3:** Write one paragraph on **radio exclusivity** ahead of Phase 3/4 — measured
      on day 1, BLE and Wi-Fi cannot both be up (16 KB largest free block with both). Apps
      will get whichever radio they touch first, and the other will return `nil, "radio in use"`.
- [ ] **Step 4: Put a date on the API freeze** and tell the participants. Everything after it
      is additive only.
- [ ] **Step 5:** Re-measure free internal DRAM at boot and record it. Compare against the
      139,711-byte figure that applies once both radio stacks are compiled in. If Phase 2
      has eaten more than a few KB, investigate before Phase 3.
- [ ] **Step 6: Commit and push.**

---

## Verification

- Every capability ships an example app in `apps/` that prints its readings over serial, so
  it can be checked without looking at the screen.
- **Values must be proven real, not stubbed** — the specific sanity checks are written into
  each task above because "it returned a number" is not evidence.
- Memory at boot recorded after each task; a drop over a few KB gets investigated.
- Launch/exit each example 10× and confirm internal DRAM returns to baseline. Note Phase 1
  measured a ~84 bytes/cycle drift that is still unexplained — see the ledger.
- `smoke.lua`: calls every module once with the SD card present, asserting nothing raises.
  Its twin runs with the card absent, asserting everything degrades to `nil, msg`.
  **This is the only check for the degrade-don't-raise rule, and none exists yet.**

## Risks

1. **Phase 1 is not hardware-verified yet.** Tasks 8–11 there are compile-only. Do not start
   here until they pass; otherwise a failure could originate in either phase.
2. **The QMI8658 pedometer may not be exposed** by the Waveshare driver, which would remove
   the "nearly free step counter" premise. Task 3 Step 1 settles it before anything depends on it.
3. **Writing two I²C drivers from scratch** (PCF85063, AXP2101) is the largest unknown here.
   Both are simple register devices, but neither has a component to lean on.
4. **`store` on FAT plus power loss.** Mitigated by temp-file-and-rename; verify it.
5. **The 24 KB NVS partition** is reserved for Wi-Fi credentials and BLE bonds. Keep `store` out of it.
