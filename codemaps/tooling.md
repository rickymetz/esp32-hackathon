> Generated: 2026-08-24 | Token-lean format for LLM context

# Host tooling and the simulator

Both exist for one reason: **verify UI and audio work without a human at the
board.** There is one board and several people.

## `tools/` — talk to the device

All Python 3, pyserial only. All glob `/dev/cu.usbmodem*` (this board's native
USB re-enumerates under a different name; never hardcode the port).

| Tool | Lines | Purpose |
|---|---|---|
| `drive.py` | 185 | command chain: `push run stop pwr tap swipe sleep shot stats` |
| `push.py` | 146 | PUSH protocol: CRC, folder apps, icon build; `--list` / `--delete` |
| `screenshot.py` | 97 | SHOT → PNG (stdlib zlib; no Pillow) |
| `stats.py` | 149 | decodes STATS: heap low-water, per-task CPU, stack headroom |
| `read_serial.py` | 51 | reset + capture console for N seconds |
| `soak.py` | 108 | long run, watch memory via MEM |
| `chaos.py` | 146 | cycle apps, poke randomly, watch memory |
| `hear.py` | 72 | records the Mac mic, Goertzel filter — proves the speaker sounds |
| `check_docs.py` | 80 | font list / symbol roster / worked example vs source |
| `png2icon.py` | 251 | PNG → LVGL RGB565 `.bin` (128px default) |
| `gen_app_icons.py` | 372 | generates the compiled-in icon set |
| `debug.sh` | 48 | JTAG/gdb over the same USB cable |
| `package_firmware.sh` | 104 | release archive + MANIFEST |

```bash
python3 -m venv .venv && ./.venv/bin/pip install -r tools/requirements.txt
./.venv/bin/python tools/drive.py push apps/myapp.lua : run myapp.lua : sleep 1 : shot out.png
```

`drive.py` **exits non-zero when any step fails** — it is the CI/agent contract.
Failures are sticky across the chain. It confirms the port with `PING` when more
than one usbmodem device is present.

`MEM`'s three fields are parsed **positionally** by `soak.py` and `chaos.py`.
Widening that reply breaks both; `STATS` is the additive one.

## `sim/` — the board-free path

Compiles the launcher's **real** Lua↔LVGL bindings against desktop LVGL, so what
it renders is what the device renders.

```bash
(cd sim && ./setup.sh && ./build.sh)     # once: clones pinned LVGL 9.5 + Lua 5.5
sim/simctl.py run apps/myapp.lua : tap 184 224 : shot out.png
```

`simctl.py` mirrors `drive.py`'s verbs exactly, plus sim-only fake-sensor
injection (`accel`, `gyro`, `battery`, `rtc`, `wifi`) and `home [list|grid] [n]`
to render the launcher's own home screen with a fake app list.

**Shared with the firmware, compiled as-is:** `app_timer.c`, `app_sandbox.c`,
`launcher_home.c`, `launcher_icons.c`, `app_button.c`, the whole
`lua_module_lvgl` binding, `cap_lua`, and `fonts_lexend`.
Substituted: `display_service_sim.c`, and stubs for voice/audio/wifi/sensors.

**The sim is single-threaded.** Its FreeRTOS shim's mutex `Take` never blocks;
its *binary* semaphore `Take` sleeps out the timeout (on one thread nobody can
signal you, so a wait is a timeout by construction). That distinction keeps the
event drain loop's pacing identical to the device's — get it wrong and the loop
busy-spins.

**What the sim cannot show you**: touch imprecision, the watchdog, real sensors,
and *failure* — the stub modules always succeed, so it cannot prove you handled
the `nil, "reason"` path. Confirm on the board.

| Test | Guards |
|---|---|
| `test.sh` | every app renders (34 ok / 9 skipped) |
| `golden.py` | 35 golden frames |
| `timing_test.py` | `timer.every` accuracy, both patterns, + fixture-did-work tripwire |
| `overrun_test.py` | catch-up guard: callback overruns its period |
| `scenarios.py` | scripted assertions |
| `widget_api_test.py` | documented widget API vs bindings |
| `safety_test.py` | memory-safety cases must raise, not corrupt |
| `promises_test.py` | documented handles/paths actually do what they say |
| `store_exit_test.py` | an unsaved store is flushed on app exit |
| `fuzz.py` | random input |
| `gallery.py` | renders a contact sheet |

Both timing tests had their tolerances **calibrated by breaking the code and
re-running**, not guessed — the first `overrun_test.py` passed with the guard
deleted. When you touch either, re-verify the same way.

## Verification recipes that actually close a loop

```bash
# See a UI change
./.venv/bin/python tools/drive.py run ui_test.lua : sleep 1 : tap 184 224 : shot out.png

# Prove the speaker sounds (macOS `say` reaches the board's mic for the reverse)
./.venv/bin/python tools/hear.py 880      # ratio ~1 silent, tens+ = a real tone

# Per-task CPU across an interval (STATS is a delta; call it twice)
./.venv/bin/python tools/drive.py run x.lua : stats : sleep 5 : stats
```

**Measuring device-side latency**: a serial round-trip benchmark cannot resolve
it. Stamp `esp_timer_get_time()` at the two points in firmware and subtract — a
fixed inter-command cadence phase-locks against the LVGL task period and will
report a 3× difference between identical builds.

## CI

`.github/workflows/sim.yml` per PR: build sim → app render → scenarios → goldens
→ timing → overrun → widget API → fuzz → upload frames.
`release.yml` on a `v*` tag: ESP-IDF v5.5.5 build → `package_firmware.sh` →
GitHub release with MANIFEST provenance.
