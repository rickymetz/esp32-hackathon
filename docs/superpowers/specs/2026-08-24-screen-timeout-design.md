# Screen timeout — Design

Dim the panel after 30 seconds of inactivity, blank it after 2 minutes.

## Context

The launcher calls `bsp_display_backlight_on()` once at boot and never touches
the panel again. There is no inactivity tracking anywhere in the firmware, and
no way for the screen to turn itself off.

This surfaced as a heat complaint, and the numbers are worth recording because
they justify the feature:

| | Die temp (reported) | Actual (−7.6 °C offset) |
| --- | --- | --- |
| After ~32 min powered, panel lit, Wi-Fi driven hard | 75.6 °C | ~68 °C |
| After a power cycle, idle | 36.4 °C | ~29 °C |

~68 °C is hot to the touch (above ~50 °C is painful) and was confirmed by hand,
not just by sensor. On an OLED the panel is the dominant load, and the device had
been sitting lit and idle for half an hour. The same lit-and-idle state is also
what drains a 200 mAh cell.

## Why OLED makes this worth doing

Each pixel emits its own light, so a black pixel is an *off* pixel drawing
essentially nothing — unlike an LCD, where a backlight burns the same power
regardless. Panel power tracks roughly with **lit pixels × brightness**. The
codebase already leans on this (`launcher_main.c`: *"True black, not near-black:
… on OLED a black pixel is an off pixel"*), which is why every screen here uses
`#000000`.

Brightness is therefore a real lever, not a cosmetic one.

## What was verified before designing

A spike confirmed the mechanism rather than assuming it:

- `bsp_display_brightness_set()` is **not** a "supported boards only" stub. It
  writes register `0x51`, matching what CLAUDE.md documents.
- It returns `ESP_OK` at 100 / 50 / 10 / 0.
- **The panel visibly obeys** — confirmed by eye, with the screen blinking
  full-bright ↔ black three times. This mattered: `SHOT` renders the LVGL
  framebuffer, not the panel, so a screenshot is byte-identical at every
  brightness. No automated check can see this.
- 50% is clearly dimmer than 100% and still very legible — chosen by eye,
  replacing an invented 10%.

Also found, and it shapes the design: `bsp_display_backlight_off()` is literally
`bsp_display_brightness_set(0)`, and `backlight_on()` is `set(100)`. There is one
mechanism, not two.

## Architecture

### The ladder

```
        < 30s          30s – 2min        > 2min
state:  AWAKE     →    DIMMED       →    ASLEEP
        set(100)       set(50)           set(0)
                                         + lv_indev_enable(touch, false)
```

**Three stages, not four.** A fourth "true off" stage was considered and is not
reachable: `esp_lcd_panel_disp_on_off()` is called once inside the BSP at init
and the panel handle is never exposed by any BSP getter. Patching the managed
component would be overwritten on the next dependency update. "Render a black
screen" is not a fourth stage either — it yields the same zero emission as
brightness 0, with more moving parts.

**Brightness 0 is not zero device power**, and the spec says so plainly so nobody
plans against a wrong premise. Emission goes to ~zero — that is the part that
was cooking the board — but the CO5300 controller stays clocked, LVGL still
renders dirty regions, and the QSPI flush still runs. A clock app updating a
label every second keeps redrawing and flushing at 1 Hz behind a dark panel.
That is real but invisible work; measure before optimising it.

Three semantics the first draft left ambiguous, made explicit:

- **`keep_awake(true)` holds the panel at 100% — it suppresses the dim step as
  well as the blank.** A watch face that dimmed after 30 seconds would be
  nearly as useless as one that went dark.
- **The timer starts at boot.** A board powered on and left alone dims at 30 s
  and blanks at 2 min without anyone touching it. That is the case that
  produced the 68 °C reading, so it is the case that most needs covering.
- **Waking resets to the top of the ladder.** After BOOT, the panel is at 100%
  and the 30-second clock starts again from zero — it does not resume
  part-way through the dim window.

### Where it lives

`button_poll_task` already runs every 20 ms and already reads BOOT. It takes on
the timeout. **No new task.** Given that this session shipped an AB-BA deadlock by
introducing a second task that touched the display, adding a fourth one here is
exactly the risk not to take.

Inactivity comes from LVGL, which already tracks it — `lv_display_get_inactive_time(disp)`.
No hand-rolled activity detection.

### Waking

| Trigger | Path |
| --- | --- |
| BOOT press | `button_poll_task` already sees it; wake and re-enable touch |
| Serial `TAP` / `SWIPE` | `launcher_input_inject()` calls wake **explicitly** before injecting |
| Finger on a dark screen | Nothing — the indev is disabled |

Disabling the touch indev does double duty: it swallows the finger (so you cannot
press invisible buttons on a dark screen) *and* generates no activity, so it
cannot reset the inactivity timer.

The serial path wakes explicitly rather than inferring "someone must have woken
us" from the inactivity counter going small. The launcher owns two indevs — the
BSP's real touch (`bsp_display_get_input_dev()`) and its own synthetic one — so
telling them apart is a pointer comparison, not a special case.

This keeps `tools/drive.py` working: a chain that idles past two minutes and then
taps still functions, and `SHOT` was never affected because it reads the
framebuffer.

### `keep_awake`

A new app-facing call so a watch face, countdown or metronome can hold the screen
lit. **Reset on app exit**, via the existing `app_*_reset()` pattern used by
timers, buttons, voice and audio — otherwise a crashed watch face pins the
backlight on until reboot, which is precisely the battery failure this feature
exists to prevent.

`sim/src/` needs a no-op version, or apps calling it break headlessly. The wifi
work in this repo learned that the hard way; a parity test alongside
`sim/wifi_parity_test.py` is the cheap guard.

### `BRIGHT <pct>` serial command

Promoted from spike scaffolding to a real command. It makes a 2-minute timeout
testable in seconds, and it is four lines. Documented in the contract's serial
table alongside `PING`.

## Compatibility

**Every existing app goes dark after 2 minutes.** `apps/clock.lua`,
`faces.lua`, `metronome.lua` and `countdown.lua` are all plausibly
"should stay lit" and none of them call the new API. Audit those and add
`keep_awake` where it belongs, rather than shipping the timeout and letting
people discover it one app at a time.

`keep_awake` is purely additive to the app API. No existing call changes
behaviour.

## Out of scope

- **True panel-off** — not reachable, see above.
- **Suppressing LVGL redraw while asleep.** Real, but measure first.
- **Configurable timeouts** in `apps/settings.lua`. The constants are named;
  wiring them to the settings UI is a separate change.
- **Wake on touch.** Deliberately rejected: BOOT is the wake gesture, so a
  pocket-tap or a sleeve cannot light the panel.

## Verification

| Claim | How |
| --- | --- |
| Dims at 30 s, blanks at 2 min | Hardware, by eye, with `BRIGHT` to shortcut waits |
| BOOT wakes | Hardware, by eye |
| Finger on dark screen does nothing | Hardware: tap a known button location while asleep, confirm no launch |
| Serial TAP wakes and acts | `drive.py … : sleep 130 : tap x y : shot` |
| `keep_awake` holds the panel lit | Hardware, by eye, with a fixture app |
| `keep_awake` is released on app exit | Run the fixture, BOOT out, confirm it dims 30 s later |
| Apps still render | `sim/test.sh`, `golden.py` |
| Sim/device API parity | New parity test |

The by-eye rows cannot be automated: `SHOT` reads the framebuffer, so every
brightness produces an identical PNG. That is a real limit of this feature's
testability and is stated rather than papered over.

## Risks

1. **A wake path that misses.** If BOOT wake fails, the device looks bricked —
   dark screen, no touch response. BOOT is a direct GPIO read with no I²C
   dependency, which is the safest input available, but this path must be
   verified on hardware before merge.
2. **`keep_awake` leaking across apps**, pinning the backlight on. Mitigated by
   the reset-on-exit pattern and a test for exactly that.
3. **Existing apps going dark unexpectedly** — see Compatibility.
