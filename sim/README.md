# esp32-sim — a headless simulator for the launcher

Run and screenshot launcher apps **on your computer**, with no board attached.
It reuses the launcher's *real* Lua↔LVGL bindings compiled against desktop
LVGL, so what renders here matches the device closely — same widgets, same
Lexend theme, same 368×448 panel, same touch/event pipeline.

```
sim/simctl.py run apps/counter.lua : sleep 1 : tap 184 224 : shot out.png
```

## What it's for

- Write and iterate on apps without flashing hardware.
- Verify UI visually (PNG screenshots) and drive taps/swipes headlessly.
- A CI gate: render every app on each PR (see "CI", below).

## Quick start

```bash
cd sim
./setup.sh        # clone pinned LVGL 9.5 + Lua 5.5.0 into external/ (once)
./build.sh        # cmake + ninja -> sim/build/sim
# from the repo root:
./sim/simctl.py run apps/counter.lua : tap 184 224 : shot /tmp/out.png
```

`simctl.py` mirrors `tools/drive.py` exactly — the same command chain that
drives the board over serial drives the sim, so muscle memory transfers.
You can also call the binary directly:

```bash
./sim/build/sim --sdroot . run apps/stopwatch.lua : tap 184 230 : sleep 2 : shot sw.png
```

Verbs: `run <app.lua>`, `stop`, `tap x y`, `swipe x0 y0 x1 y1 [ms]`,
`sleep <seconds>`, `shot <out.png>`. `--sdroot DIR` sets the SD-card root that
app and `font_load` paths resolve against (default: repo root).

## How it works

The launcher already abstracts the display behind a `display_service`
component and injects synthetic touch through a dedicated LVGL indev (for
serial `TAP`/`SWIPE`). The sim slots into those same seams:

| Layer | On device | In the sim |
| --- | --- | --- |
| LVGL 9.5 | ESP-IDF component, panel via QSPI | built for host, renders into a memory framebuffer |
| Lua 5.5 core | `espressif/lua` | same upstream core, built for host |
| `lua_module_lvgl` (6.8k LOC) | as-is | **compiled as-is** against host LVGL |
| `ui` / `keyboard` | embedded blobs | loaded from the component's `.lua` files |
| `timer`, `button`, sandbox | as-is | **compiled as-is** (only need `esp_timer_get_time`) |
| `voice` (MultiNet) | mic + models | stub: `available()==false`, `cb(nil)` on use |
| display / touch | `display_service` + panel + `esp_lcd_touch` | `display_service_sim.c` + framebuffer + synthetic indev |
| ESP-IDF / FreeRTOS | real | ~150-line `shim/` (esp_err/log/timer/heap, mutex, delay) |
| `SHOT` | serial-encoded frame | `sim_display_capture_png()` → PNG |

The event pump (`sim_main.c`) reproduces the launcher's app-task loop:
`app_button_run_pending` → `app_voice_run_pending` → `app_timer_run_due` →
`lv_timer_handler` (indev, animations, refresh, event dispatch) → drain the
Lua event queue via `process_events`.

## What it does NOT simulate

The sim is faithful at the LVGL/Lua/app-contract layer, not at the hardware
layer. These still need the board:

- **Touch imprecision.** The digitizer drops small targets (the ≥200×100
  rule); the sim registers every tap exactly. A button that works in the sim
  can still be too small on hardware.
- **Voice.** MultiNet recognition is stubbed as unavailable.
- **IMU / RTC / battery / PMU**, real audio, Wi-Fi — none are present.
- **Watchdog reboots** from runaway loops / blocking C calls; timing of the
  10 s TWDT; PSRAM budgets.
- Exact **color** — the panel is RGB565 and the sim matches that, but real
  AMOLED brightness/gamma differ.

Treat a green sim run as "the logic and layout are right", then confirm feel
on hardware.

## Layout

```
sim/
  setup.sh build.sh simctl.py   # fetch deps, build, drive
  lv_conf.h                     # host LVGL config (RGB565, matches device)
  CMakeLists.txt
  shim/                         # ESP-IDF / FreeRTOS host shims
  src/
    sim_main.c                  # runner + command interpreter
    display_service_sim.c       # host display_service (display + indev + theme)
    sim_input.c                 # synthetic touch (ported from launcher)
    sim_voice.c sim_module_ui.c # voice stub, ui/keyboard loader
    sim_display.c png_write.c sim_tick.c
    main_slice{1,2,3}.c         # staged smoke tests (see git history)
  external/                     # LVGL + Lua sources (gitignored; setup.sh)
```

## CI

Because the sim is headless and self-contained, a workflow can build it and
assert every app at least loads and renders:

```bash
cd sim && ./setup.sh && ./build.sh
for app in ../apps/*.lua; do
  ./build/sim --sdroot .. run "$app" : shot "/tmp/$(basename "$app").png" || exit 1
done
```
