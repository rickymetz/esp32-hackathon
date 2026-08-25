# Performance testing and debugging

How to measure this board rather than guess at it. Three layers, cheapest
first — most work needs only the first two.

Everything here is device-only. The simulator cannot help: it has no
FreeRTOS, no real heap pressure, and 64-bit Lua where the device has 32-bit.
`sim/timing_test.py` covers timer *accuracy* on the host and that is all.

---

## 1. `STATS` — the numbers you usually want

```bash
./.venv/bin/python tools/stats.py            # one snapshot
./.venv/bin/python tools/stats.py 30 1       # 30 samples, 1s apart
./.venv/bin/python tools/stats.py 60 0.5 run.csv   # ...and write CSV
```

It also chains inside `drive.py`, which is how you measure a *specific*
interaction rather than the idle device:

```bash
./.venv/bin/python tools/drive.py run stopwatch.lua : stats : sleep 5 : stats
```

**CPU is a delta between consecutive `STATS` calls, not a since-boot
average.** That is deliberate — a since-boot average buries exactly the
spike you are hunting. It also means the *first* call in any session is
meaningless; `tools/stats.py` always throws away a priming sample for you,
and in a `drive.py` chain you want `stats` twice, as above.

Both cores feed the run-time total, so the CPU column sums to roughly
**2000 permille (200%)** across a snapshot, idle tasks included. A task at
100% is saturating one core, not the chip.

### What each field is for

| Field | Why it matters |
| --- | --- |
| `psram free` / `internal free` | The instantaneous figure. Same as `MEM`. |
| `psram min` / `internal min` | **All-time low-water mark.** `MEM` cannot report this, so a run that came within a few KB of the floor and recovered reads identically to one that never got close. This is the number that actually bounds headroom. |
| `internal largest` | Largest contiguous block. Fragmentation fails allocations long before the total looks tight — Wi-Fi and LVGL both want internal DRAM. |
| `cpu_permille` | Per task, over the interval. |
| `stack_free` | Minimum stack headroom ever seen for that task, in bytes. The only evidence for whether `APP_TASK_STACK` (32 KB) is right rather than merely untested. |

`MEM` still exists and is unchanged — `tools/soak.py` and `tools/chaos.py`
parse its three fields positionally, and widening it would have broken both.
`STATS` is additive.

---

## 2. The on-screen FPS overlay

`CONFIG_LV_USE_SYSMON` + `CONFIG_LV_USE_PERF_MONITOR` paint LVGL's own
frame-rate and CPU figure into the corner of the live screen.

**`SHOT` does not capture it, and cannot.** `handle_shot()` calls
`lv_snapshot_take(lv_screen_active())`, but sysmon builds its label on the
*system layer* — `lv_label_create(lv_display_get_layer_sys(disp))`, at
`lv_sysmon.c:91`. The system layer is a sibling of the screen, not a child,
so a snapshot of the active screen cannot contain it. The overlay is
visible on the physical panel only.

Two consequences worth knowing:

- Golden-frame tests and UI screenshots are **unaffected** by the overlay,
  which is convenient — it cannot contaminate a diff.
- If you want frame rate the harness can *read*, the overlay is the wrong
  mechanism. `CONFIG_LV_USE_PERF_MONITOR_LOG_MODE=y` emits it to the log
  instead, in a parseable form (`sysmon: N FPS (refr_cnt ...) ... CPU N%%`)
  — but it goes through `LV_LOG`, so it also needs `CONFIG_LV_USE_LOG=y`,
  which is currently off. That is a two-symbol change plus a rebuild, and
  it turns LVGL logging on globally.

The simulator has its own `lv_conf.h` with sysmon off, so CI is unaffected
either way.

`CONFIG_LV_USE_MEM_MONITOR` is deliberately **not** enabled: it reports
`lv_mem_monitor()`, whose core is a no-op under `CONFIG_LV_USE_CLIB_MALLOC`,
so it would paint a permanent `0/0`. `STATS` reports the real heap.

---

## 3. JTAG, gdb, and real breakpoints

**You already have a JTAG port.** `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`
means the console runs on the S3's built-in USB-Serial/JTAG peripheral,
which exposes a CDC serial interface *and* a JTAG interface on one USB
device. No probe, no wiring.

```bash
tools/debug.sh check       # confirm OpenOCD sees the chip
tools/debug.sh openocd     # leave this running
tools/debug.sh gdb         # second terminal
```

Two things that will bite:

1. **Do not run `drive.py`, `push.py` or `idf.py monitor` while
   single-stepping.** Halting the CPU stops the firmware, so the serial task
   stops answering and those tools just time out.
2. **A halted target looks exactly like a crashed one from the host.** If the
   board seems dead after a debug session, `continue` in gdb or quit OpenOCD
   before reaching for the physical BOOT/PWR recovery dance.

Beyond breakpoints, `CONFIG_APPTRACE_DEST_JTAG` plus SEGGER SystemView gives
a per-task timeline over the same link. That is a bigger lift and nothing
here depends on it.

---

## The cost of all this

Instrumentation is not free, and it comes out of the scarcest resource on
the board — internal DRAM. `CONFIG_FREERTOS_USE_TRACE_FACILITY` widens every
TCB; run-time stats add a counter per task.

Measured on hardware as a clean A/B — same source, same toolchain, two
builds differing only in the five CONFIG symbols, each flashed and sampled
at launcher idle:

| | Off | On | Delta |
| --- | ---: | ---: | ---: |
| PSRAM free | 5,082,680 | 5,082,112 | **−568 B** |
| Internal free | 192,151 | 179,959 | **−12,192 B** |
| Largest contiguous internal | 73,728 | 73,728 | **0** |

**About 12 KB of internal DRAM, and no change at all to the largest
contiguous block.** That second row is the one that mattered: fragmentation
is what fails allocations here, and instrumentation does not touch it. The
12 KB is roughly 6% of free internal DRAM.

Do not compare against a figure taken from a *different* firmware build —
the first attempt at this measurement did, and read as instrumentation
*freeing* 23 KB, because the board was running an older launcher. Only an
A/B on one source tree means anything.

If you ever need that 12 KB back, dropping the three `CONFIG_FREERTOS_*`
symbols recovers it; `STATS` keeps working and reports
`cpu unavailable` instead of failing.
