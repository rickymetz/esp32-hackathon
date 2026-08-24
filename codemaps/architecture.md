> Generated: 2026-08-24 | Token-lean format for LLM context

# Architecture

A shared app launcher for the Waveshare ESP32-S3-Touch-AMOLED-1.8. Apps are Lua
scripts on the SD card; installing one is a file copy, not a reflash.

Three languages, one system:

| Part | Language | Build |
|---|---|---|
| `launcher/` | C (ESP-IDF v5.5.5) | `idf.py build` → `launcher.bin` |
| `apps/` | Lua 5.5 | none — copied to `/sdcard/apps/` |
| `sim/` | C against desktop LVGL | `sim/build.sh` → `sim/build/sim` |
| `tools/` | Python 3 (pyserial only) | none |

## Task model — the thing to understand first

Four FreeRTOS tasks. `CONFIG_FREERTOS_HZ=1000`.

```
taskLVGL (prio 4)          lua_app (prio 5)         serial_push (prio 4)   buttons (prio 6)
  esp_lvgl_port              one per running app       line protocol         20ms poll
  holds DISPLAY LOCK         own Lua VM (PSRAM)        over USB-CDC          BOOT + PWR
  across lv_timer_handler    32KB stack                8192B stack
        |                          ^
        | enqueues events          | drains them
        +----> event queue --------+
               (binary semaphore wakes the consumer)
```

**LOCK ORDER — display lock is always outermost.** Every LVGL event callback
runs inside the display lock (esp_lvgl_port holds it across `lv_timer_handler()`)
and then takes `s_app_mutex`. Any other task needing both must use that order,
or post work to the LVGL task with `lv_async_call`. Taking `s_app_mutex` first
and then blocking on the display lock is an AB-BA deadlock with any concurrent
tap, and both tasks block cleanly so no watchdog fires. This shipped once.

**Lua callbacks only queue.** The LVGL trampoline enqueues; `lvgl.process_events()`
on the app task drains. The launcher pumps it so apps stay declarative — an app
builds its UI and *returns*. An app with its own `while true` loop freezes the device.

## Module graph

```
app_main (launcher_main.c)
  ├─ release_panel_reset()      TCA9554 EXIO1/EXIO2 — panel is dark without this
  ├─ bsp_display_start()        BSP: CO5300 + CST816
  ├─ display_service_attach()   seam the sim swaps
  ├─ lua_module_lvgl_register*  ~45 LVGL metatables
  ├─ app_{timer,button,audio,voice,sensors,wifi}_register()
  ├─ lua_module_{store,ui}_register()      ui.lua/keyboard.lua require() the above
  ├─ app_registry_scan()        mount SD, list apps/, sort
  ├─ build_launcher_ui()  ──→   launcher_home.c (shared with sim)
  ├─ button_poll_task
  └─ serial_push_start()   ──→  serial_push.c
```

**Registration order matters**: `cap_lua` opens modules in registration order,
and the embedded-Lua modules (`ui`, `keyboard`) `require()` lvgl/timer/voice at
load time, so those must register first.

## App lifecycle

```
tap row / RUN <id>  →  s_current_app = copy   (never a pointer into s_apps[])
                    →  xTaskCreate(lua_app_task)
                       ├─ lua_newstate(PSRAM allocator)
                       ├─ open libs, modules, seed RNG from esp_random()
                       ├─ app_sandbox_apply()   removes debug/package/os.exit/os.execute
                       ├─ luaL_loadfile + pcall  (traceback handler)
                       └─ pump loop: buttons → voice → timers → process_events
BOOT / STOP         →  atomic stop flag → Lua interrupt hook unwinds
exit                →  clear s_app_task, show launcher, rebuild if s_home_stale
```

Verified: launch/exit returns PSRAM to the same free figure — no leak.

## Data flow: installing an app

```
tools/push.py ──PUSH name len crc──> serial_push.c
                 base64 body           ├─ verify CRC
                 ENDPUSH               ├─ app_registry_write_app()  temp + rename
                                       │    └─ scan_locked()  rescan + qsort
                                       ├─ signature changed? → launcher_refresh_ui()
                                       │    └─ lv_async_call ──> LVGL task rebuilds
                                       └─ PUSH_OK
```

Rebuild is **deferred** (stale flag) while an app runs or the info sheet is open,
and replayed when the launcher next becomes visible.

## Build & flash

```bash
. ~/esp/esp-idf/export.sh
export PORT=$(printf '%s\n' /dev/cu.usbmodem* | head -1)   # glob, not ls
cd launcher && idf.py build && idf.py -p $PORT flash monitor
```

`sdkconfig.defaults` is not optional — `CONFIG_SPIRAM_MODE_OCT=y` in particular
is not the IDF default, and without it 8MB PSRAM silently fails to init.
`sdkconfig` is generated and gitignored; delete it to re-apply changed defaults.

Partitions (`partitions.csv`, 16MB flash): `factory` 4MB (1MB default is too
small), `storage` 4MB FAT, `model` 4MB (esp-sr looks for that literal name).

## Testing

| Layer | Command | Covers |
|---|---|---|
| Sim apps | `sim/test.sh` | every app renders (34 ok / 9 skipped) |
| Goldens | `sim/golden.py` | 35 frame regressions |
| Timer grid | `sim/timing_test.py` | `timer.every` accuracy both patterns |
| Catch-up guard | `sim/overrun_test.py` | callback overruns its period |
| Widget API | `sim/widget_api_test.py` | docs match bindings |
| Fuzz | `sim/fuzz.py` | random input |
| Docs | `tools/check_docs.py` | font list, symbols, worked example |
| Hardware | `tools/drive.py` | real board, real touch pipeline |

CI: `.github/workflows/sim.yml` (per PR), `release.yml` (on `v*` tag).

## Dead ends — do not re-explore

- **`lv_binding_lua` does not exist.** No repo, not in the LVGL org.
- **`esp-brookesia` unusable**: master needs unreleased ESP-IDF 6.2; the
  IDF-5.5 branch has no `runtime/`.
- **usb_serial_jtag driver for SHOT throughput**: tried, 4–5× WORSE
  (1.5s → 7–10s). Measured dead end, not an untried idea.
