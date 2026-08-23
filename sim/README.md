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
`pwr [down|up|long]` (inject the PWR button — quick click by default),
`sleep <seconds>`, `shot <out.png>`, `check <out.png>`,
`home [n]` (render the launcher's own home screen with a fake app list —
`n` apps, `0` for the empty state; see *The launcher home*, below).

**Fake-sensor injection.** The board has no such controls; these set the sim's
stub readings so degraded and dynamic UI paths — a tilted level, a low battery,
an unset clock, a failed connection — become reproducible without hardware.
Issue them *before* `run` to reach an app's load-time read, or *after* `run` for
apps that poll on a timer (each sim invocation is a fresh process, so nothing
leaks between runs):

| Verb | Effect |
| --- | --- |
| `accel x y z` | set the IMU acceleration in g (e.g. `0.5 0 0.866` ≈ 30° tilt) |
| `gyro x y z` | set the IMU angular rate in °/s |
| `battery pct [charging] [ext]` | set the gauge; `pct` `-1` → "gauge not ready" |
| `rtc unset` | make `rtc.now()` report `"rtc not set"` |
| `rtc set y mo d h mi s [wday]` | set the wall clock |
| `wifi ok` / `wifi fail` | how the next `wifi.connect()` resolves |

Options: `--sdroot DIR` sets the SD-card root that app and `font_load` paths
resolve against (default: repo root). `--timeout SECONDS` sets the per-app
watchdog budget (default 10; a runaway app is stopped, then killed if it
won't unwind).

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
| `rtc` / `imu` / `battery` | PCF85063A · QMI8658 · AXP2101 over I2C | stub: fixed clock (14:30 Sat 22 Aug, settable), board-flat accel `(0,0,1)g`, 76% battery |
| `audio` | ES8311 over I2S | stub: same arg validation, playback no-ops, `available()==true` |
| `wifi` | esp_wifi + SNTP | stub: `connect()`→`connecting`→`connected` over 3 status polls, then a fake IP + NTP-set clock |
| display / touch | `display_service` + panel + `esp_lcd_touch` | `display_service_sim.c` + framebuffer + synthetic indev |
| ESP-IDF / FreeRTOS | real | ~150-line `shim/` (esp_err/log/timer/heap, mutex, delay) |
| `SHOT` | serial-encoded frame | `sim_display_capture_png()` → PNG |

The event pump (`sim_main.c`) reproduces the launcher's app-task loop:
`app_button_run_pending` → `app_voice_run_pending` → `app_timer_run_due` →
`lv_timer_handler` (indev, animations, refresh, event dispatch) → drain the
Lua event queue via `process_events`.

### The launcher home

The sim runs individual apps, but the launcher's *own* home screen (the app
list) is the most-seen surface. Its LVGL builder was factored out of
`launcher_main.c` into `launcher/main/launcher_home.c` — pure LVGL, no BSP or
app registry — so both the board and the sim call the same
`launcher_home_build()`. The device passes its real app-registry accessor and
row/refresh callbacks; the `home [n]` command passes a fixed fake app list and
no callbacks, so the rendered home is the real launcher UI, board-free.
(Because CI here builds the sim, not the firmware, compiling `launcher_home.c`
in the sim is also what verifies that shared code compiles.)

## What it does NOT simulate

The sim is faithful at the LVGL/Lua/app-contract layer, not at the hardware
layer. These still need the board:

- **Touch imprecision.** The digitizer drops small targets (the ≥200×100
  rule); the sim registers every tap exactly. A button that works in the sim
  can still be too small on hardware.
- **Voice.** MultiNet recognition is stubbed as unavailable.
- **Real sensor data.** `rtc`/`imu`/`battery`, `audio` and `wifi` are faked
  (`src/sim_sensors.c`, `src/sim_audio.c`, `src/sim_wifi.c`): a fixed clock
  (14:30 Sat 22 Aug), a dead-flat IMU, 76% battery, silent audio, and a
  network that always connects. Deterministic so apps render and goldens hold,
  but nothing measures anything — confirm real readings, sound, and a live
  connection on the board. Their *defaults* can be overridden per run with the
  fake-sensor injection verbs (above), which is how the degraded paths get
  tested; the fully live behaviour still needs hardware.
- **Module failure — partly.** The injection verbs reach some degraded paths
  the device shows (`rtc unset` → `"rtc not set"`, `battery -1` → "gauge not
  ready", `wifi fail` → `"failed"`). Others — a NAKing I2C sensor, a mid-read
  hardware fault — the stubs can't reproduce; those still need the board.
- **Watchdog reboots** from runaway loops / blocking C calls; timing of the
  10 s TWDT; PSRAM budgets.
`sim/timing_test.py` guards the timer-accuracy bug class (see
`sim/fixtures/timing.lua`): it paces the same duration with both the wrong
pattern and the right one and asserts they are still distinguishable. Runs in
CI.

- **Timer dispatch latency, to scale.** Timers are inaccurate in the same
  *direction* here as on the board -- a periodic timer re-arms after its
  callback, so it always runs slow -- but not to the same *degree*. Measured
  with a 100 bpm metronome: the simulator overshoots by ~2.7 ms per tick, the
  board by ~24 ms. So a timing bug is roughly an order of magnitude quieter
  here, and one that looks like harmless jitter in the sim can be plainly
  wrong on hardware. The sim is still the right place to *find* these -- add a
  `print(timer.now_ms())` and read the intervals off stdout -- just don't read
  its margins as the real ones.
- Exact **color** — the panel is RGB565 and the sim matches that, but real
  AMOLED brightness/gamma differ.
- **Filesystem paths.** `--sdroot` resolves the app and `font_load` paths
  (via the binding), but raw `io.open` goes straight to the host FS — an app
  that persists to `/sdcard/...` won't find that path on the host. Test
  persistence on the board.

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
    sim_audio.c sim_wifi.c sim_sensors.c  # audio / wifi / rtc+imu+battery stubs
    sim_display.c png_write.c sim_tick.c
    main_slice{1,2,3}.c         # staged smoke tests (see git history)
  external/                     # LVGL + Lua sources (gitignored; setup.sh)
```

## Testing

`sim/test.sh` render-tests every app in `apps/` (skipping the intentional
error/runaway/no-UI fixtures): it runs each app, lets it settle, and asserts
the frame is non-blank. Shots land in `sim/build/shots/`.

```bash
sim/test.sh              # exits nonzero if any app crashes or draws nothing
```

The sim's own hand-written C (the PNG writer, the synthetic-input queue) has
unit tests:

```bash
ninja -C sim/build sim_tests && sim/build/sim_tests
```

`sim/scenarios.py` goes a step past "non-blank": it drives an app through an
interaction and asserts a specific screen region's colour (a slider actually
recolours the swatch, a toggle actually toggles, the reaction pad actually
turns green) — catching binding/state regressions a blank-check can't.

```bash
sim/scenarios.py
```

`sim/golden.py` is the full-frame net for **structural** regressions: it renders
each curated app to its canonical settled frame, downscales 4× to a thumbnail
(box-averaging smooths anti-aliasing), and compares against a committed golden
under `sim/golden/`. The comparison is perceptual — a thumbnail cell counts as
changed only past a per-channel tolerance, and a frame fails only if more than
~1.2% of cells change. That budget catches the big stuff a hand-picked pixel
would miss (a blanked or wholly different frame, a background/theme swap, a large
recolour, a major layout break) while surviving cross-machine render wobble; it
is *not* a pixel-diff, so a single vanished caption or a few-pixel nudge can slip
through — guard those specific elements with a `scenarios.py` probe. The
thumbnails are ~2 KB each and render in GitHub diffs, so a deliberate update is
reviewable by eye.

```bash
sim/golden.py                 # compare; exits nonzero on drift
sim/golden.py --update        # regenerate every golden after an intended change
sim/golden.py --update clock  # or just the named app(s)
```

Random- or animation-driven apps (dice, simon, reaction, breathe, metronome)
aren't golden-tested — their frames aren't deterministic; scenarios.py covers
those.

## CI

`.github/workflows/sim.yml` runs all of this on every PR that touches the sim,
the apps, or the bindings they depend on: it builds the sim on a plain Linux
runner (no ESP-IDF, no board), runs the unit tests, `test.sh`, the scenario
assertions and the golden-frame check, and uploads the rendered frames as an
artifact. The LVGL/Lua checkout is cached between runs.

## Sanitizers

The runner is hand-written C, so it's worth checking under ASan/UBSan:

```bash
cmake -S sim -B sim/build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
ninja -C sim/build-asan sim
ASAN_OPTIONS=detect_leaks=1 sim/build-asan/sim --sdroot . run apps/counter.lua : tap 184 224
```

The suite (app-switching, error paths, keyboard, the watchdog) runs clean.
