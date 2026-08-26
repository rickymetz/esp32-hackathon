# Checks that need hands on the board

Everything else in this repo is verifiable from a keyboard: `tools/drive.py`
drives touch, `BOOT`, screenshots, memory and now the console log. These are the
things it genuinely cannot reach, and **why** — so nobody spends an afternoon
trying to automate one, as happened with the FPS overlay.

Two minutes, next time you are at the device.

---

## 1. The card-less boot

**Proves:** built-in apps (#34) do the job they exist for.

```
1. Power the board off  (hold PWR >= 6 s)
2. Remove the microSD card
3. Power on
```

**Expect:** the watch face as usual, then BOOT to the app list. It should show
**five apps** — Settings, Counter, Stopwatch, Countdown, Flashlight — with
"No SD card" under the "Apps" header and **no list/grid toggle** in the corner.
Open Settings; it must work, because every device setting lives in NVS.

Then, still powered on, **insert the card and tap Refresh** at the bottom of
the list. The full app set should appear and the "No SD card" note go away.

**Why not automatable:** a mount failure cannot be simulated over serial —
`bsp_sdcard_mount()` either finds hardware or does not. The simulator renders
the screen (`sim/build/sim home nocard`, golden `home_nocard`) and the
shadow/resurface logic is verified on-device, but the mount path itself is not.

**If it fails:** the failure mode to watch for is an *empty* list rather than
five apps, which would mean the built-ins are not being seeded before the mount
attempt in `scan_locked()`.

---

## 2. The FPS overlay

**Proves:** `lvgl.perf_overlay()` and the Settings toggle actually drive LVGL.

The toggle is currently **on**. Look at the panel: there should be a small
frame-rate/CPU readout in the **bottom-right corner**, over whatever is on
screen.

Then Settings → Display & sound → **FPS overlay** off, and confirm it goes away.

**Why not automatable — and this one is absolute:** `SHOT` calls
`lv_snapshot_take(lv_screen_active())`, while sysmon builds its label on the
display's **system layer** (`lv_sysmon.c`), which is a *sibling* of the active
screen rather than a child. No screenshot can ever contain it. This is not a
harness limitation to be fixed later; it is the object graph.

**If it fails:** the switch will still flip and persist (that part is verified),
so the symptom is a setting that remembers a state nothing acts on — check the
binding in `lua_lvgl_core_widgets.c`.

---

## 3. Worth a glance while you are there

- **BOOT with the screen dimmed.** Leave an app for 30 s until the panel dims,
  then one press should exit to the face. This was broken (it took two presses)
  and is fixed and verified — but it is the control everything else depends on.
- **A tap on a fully asleep screen** (2 min idle) should do nothing but wake it.
  Only BOOT navigates from black.

---

## What would remove the need for most of this

A **second console on UART0** — GPIO 43/44 are free, and a USB-TTL adapter is a
few pounds. It would give a channel readable during boot, during a crash
backtrace, and while the harness owns USB.

`LOG` (added in #13) lets you read the console *after the fact* over the same
port, which covers most debugging. What it cannot do is show you a board that
has wedged its USB — the failure that cost an afternoon and needed a physical
PWR/BOOT recovery, with no way to see why.
