# Apps

Lua apps for the launcher — one file each, installed to `/sdcard/apps/`. See
`docs/APP_CONTRACT.md` for the full API; this is a map of what's here and which
technique each example shows.

## Develop without a board

The headless simulator (`sim/`) runs these on your computer and screenshots
them — the fast way to iterate. Build once, then:

```bash
sim/simctl.py run apps/counter.lua : tap 184 224 : shot out.png
```

`sim/test.sh` render-tests every app at once. See `sim/README.md`.

## Examples worth copying

Each of these is a complete, working app that leans on a different part of the
contract — start from the one closest to what you're building.

| App | Shows |
| --- | --- |
| `counter.lua` | The minimal shape, on `ui` helpers — the file to copy |
| `stopwatch.lua` | `timer.now_ms()` timestamps (never tick-counting), `lvgl.font(60)`, PWR-as-lap |
| `tally.lua` | The PWR-as-accelerator pattern + a confirm-gated (destructive) reset |
| `dice.lua` | `math.random`, and `ui.corner_button`'s `{ button, visual }` return shape |
| `countdown.lua` | A `ui.stepper` as both setter and live readout; deadline-based timing; a toast at zero |
| `tip.lua` | `require("keyboard")` number entry; steppers; a scrollable result row |
| `reaction.lua` | Randomised `timer.after`, and measuring elapsed time with `timer.now_ms()` |
| `flashlight.lua` | A full-screen tap target under a slider; recoloring the screen |
| `metronome.lua` | A repeating beat with retempo-on-the-fly; a flash via `timer.after` |
| `color.lua` | Live-updating sliders and a contrast-aware hex readout |
| `simon.lua` | A game state machine with chained timers on a 2&times;2 grid |
| `breathe.lua` | A paced size animation driven by a 50&nbsp;ms timer |
| `sign.lua` | `require("keyboard")` text entry; auto-sizing centred display |
| `settings.lua` | The user-controllable font scale, with a live preview |
| `level.lua` | `require("imu")` — a bubble level from the accelerometer, polled on a timer |
| `tone.lua` | `require("audio")` — a tone generator: a frequency slider driving `audio.tone`, PWR mirrors Play |
| `hello_world.lua` | A gentle tour of widgets |

Two gotchas these examples were written to avoid — both bite silently:

- **A `local`'s scope starts *after* its declaring statement.** A closure that
  refers to the very local it's being assigned to (a timer handle, a
  `corner_button` result) sees a `nil` global instead. Declare first, assign
  second — `dice.lua` and the `timer` docs show the fix.
- **`ui.stepper`'s `label` is a format string** (`"%d bpm"`), and a wide value
  can collide with its own +/- buttons — `metronome.lua` keeps it to `"%d"`.

## Test fixtures (not shippable apps)

These exist to exercise the launcher's own edges and are skipped by
`sim/test.sh`:

- `broken.lua`, `cb_error.lua`, `deep_error.lua` — error paths / the error screen
- `hook_bypass.lua`, `trim_check.lua` — the sandbox (stripped stdlib, hook)
- `runaway_bare.lua`, `runaway_coro.lua`, `runaway_pcall.lua` — the watchdog
- `headless.lua` — an app with timers but no UI
- `input_test.lua`, `tick_test.lua`, `timer_reuse.lua`, `timer_slot.lua`,
  `voice_test.lua`, `spell_test.lua`, `ui_test.lua` — module regression checks.
  These DO run in `sim/test.sh` (they render headlessly); only the
  error-path/watchdog fixtures above are in its SKIP list.
