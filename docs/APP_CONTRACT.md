# App Contract

Everything you need to write an app. If you are writing an app rather than working on the
launcher, this is the only document you need.

**Verified against the running launcher** — the worked example (`apps/counter.lua`) has
been executed on the board repeatedly, not sketched. The `timer` module and font-loading
calls are checked directly against the launcher's own C sources
(`launcher/main/app_timer.c`, `launcher/components/lua_module_lvgl/src/lua_lvgl_font.c`),
cited where it matters.

---

## The shape of an app

One Lua file. Copy `apps/counter.lua` and edit it.

```lua
local lvgl = require("lvgl")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

local label = lvgl.label(scr, {
    text = "Hello",
    align = "center",
    text_color = "#ffffff",
})

scr:load()
```

That is a complete, working app.

**`buffer_lines` does nothing. Copy the 40 and move on.**

It is accepted and ignored. The render buffer belongs to the BSP — `bsp_display_start()`
allocates it once, in the launcher, before any app exists — and no app can influence it.
`lvgl.init` collects `buffer_lines`, `tick_ms` and `task_period_ms`, packs them into a
config struct, and the display service never reads that struct.

This document used to tell you to raise `buffer_lines` if a full-screen animation visibly
tore. **That advice was wrong** — the value was discarded, so anyone who followed it
changed nothing and went looking for the fault somewhere else. If you see tearing, say so
rather than tuning a number that is thrown away; making the buffer app-settable is
launcher work, and on a board whose largest contiguous internal block is ~73 KB, letting
one app resize a shared buffer at runtime is a way to destabilise the whole device — which
is why it is not simply switched on.

The three keys stay accepted so no existing app breaks.

### The rules that matter

1. **Build your UI, then return.** Do **not** write a `while true` loop or call
   `lvgl.run()`. The launcher pumps the event loop for you. An app that loops forever
   freezes the device, including the way back to the launcher. Need something recurring?
   Use the `timer` module (below), not a loop.
2. **Make your own screen** with `lvgl.create_screen()` and `scr:load()`. Never reach for
   the launcher's screen. When your app exits, the launcher deletes your screen and every
   widget on it — you do not need to clean up widgets.
3. **Touch is not pixel-accurate.** Measured on this hardware: a 240×120 button catches
   every tap, while a 180×56 one dropped roughly half. **Aim for ≥ 200×100, and never go
   below 88×88.** Between those two you are in a grey band that works but is not
   comfortable — fine when the layout forces it (two buttons side by side on a 368 px
   screen land near 164×104), so long as you keep the height at 104 or more. If your
   buttons feel broken, this is almost certainly why.
4. **Errors are visible, not fatal.** A Lua error stops your app and shows a red,
   full-screen error with a traceback, then waits for BOOT — or `STOP` over serial — before
   returning to the launcher. It does not vanish silently, and it does not reboot the
   board. This is different for an error *inside an event callback* (a `btn:on(...)`
   handler): that is caught, logged to serial under the tag `lua_lvgl_evt`, and your app
   keeps running — see Events, below.
5. **BOOT (the top-right button) exits your app — almost always instantly.**
   It is hardware: no app can consume or override it. It lands on the *watch face*,
   which is home; the app list is one more BOOT press from there. (So BOOT is a
   three-way toggle: app → face → app list → face.) (PWR, bottom right, belongs to your
   app — see the `button` module.) Under the hood, BOOT works by interrupting the Lua
   interpreter, which is fast but can't reach everywhere: a tight loop **inside a
   coroutine body, or inside a blocking C call**, is invisible to that interrupt. Those get
   caught by a watchdog instead, which reboots the *entire board* — not just your app —
   after about 10 seconds. Either way you get back to a working launcher, but the
   coroutine/C-call path is slower and noisier. Practical takeaway: never write a
   `while true` loop anywhere in your app, coroutine or not (rule 1), and use `timer` for
   anything periodic.

---

## Your first app, start to finish

```bash
# 1. Once per clone -- the tools need pyserial.
python3 -m venv .venv && ./.venv/bin/pip install -r tools/requirements.txt

# 2. Start from the template.
cp apps/counter.lua apps/myapp.lua

# 3. Install it over USB (no card shuffling, no reboot).
./.venv/bin/python tools/push.py apps/myapp.lua

# 4. Launch it -- tap its row on the device (it is already in the list), or:
./.venv/bin/python tools/drive.py run myapp.lua
```

Edit, re-run steps 3 and 4, repeat. That is the whole loop — or chain it in one command:

```bash
./.venv/bin/python tools/drive.py push apps/myapp.lua : run myapp.lua : sleep 1 : shot out.png
```

## Develop without the board

There is one board and several of us. You do **not** need it to write an app: a headless
**simulator** compiles the launcher's real Lua↔LVGL bindings against desktop LVGL, so what
it renders is what the device renders.

```bash
# once:
(cd sim && ./setup.sh && ./build.sh)

# then, from the repo root:
sim/simctl.py run apps/myapp.lua : tap 184 224 : shot out.png
```

You get a PNG of your app and can tap, swipe, and screenshot it in a chain. Every module
is available: `rtc` follows your computer's clock, and `battery`, `imu`, `audio` and `wifi`
return fixed, plausible values so a layout renders.

What the simulator **cannot** show you is anything about the hardware itself: touch
imprecision, the watchdog, real sensor readings — and, importantly, **failure**. On the
device these modules degrade, returning `nil, "reason"`; in the simulator they always
succeed, so it cannot prove you handled that. Confirm on the board. Details in
`sim/README.md`.

## Installing your app

The fast path, no SD card shuffling:

```bash
./.venv/bin/python tools/push.py apps/myapp.lua
```

This sends the file to the board over USB, rescans the app list, and asks the launcher to
rebuild its home screen — so the app normally appears on the device without a Refresh tap
or a reboot.

"Normally", because the rebuild is deferred when the launcher is not the thing on screen:
if an app is running, or the app-info sheet is open, the screen is rebuilt when you next
return to the launcher rather than underneath what you are looking at. That is the common
case in a `push : run` loop, where every push after the first lands while the previous app
is still up. Either way the change is never lost, and Refresh remains for a card you
swapped rather than pushed to.

Tap the app's row, or send `RUN myapp.lua` over serial (see Debugging) to launch it without
touching the panel.

You can still do it by hand: copy the `.lua` file to `/apps/` on the microSD card. That is
the same folder as `/sdcard/apps` on the device — one name is how you see it when the card
is in a computer, the other is how the device's filesystem sees it, but there is only one
folder. The launcher scans it at boot and after every push, and lists every `.lua` file it
finds. **The filename becomes the name in the list**: `weather_clock.lua` shows as
"Weather clock".

### Some apps are built into the firmware

A handful of apps are baked into the binary so the device is useful **with no SD
card**: `settings.lua`, `counter.lua`, `stopwatch.lua`, `countdown.lua`,
`flashlight.lua`. They appear in the list like any other app and run the same
way; the app-info sheet marks them **Built-in** and offers no Delete, and
`DELETE` over serial answers `DELETE_ERR builtin`.

**A card app with the same filename shadows its built-in.** So pushing your own
`settings.lua` works exactly as before — the card copy is what runs. Delete that
copy and the built-in comes back on the next scan, which is how you undo a push
that broke something. You cannot get into a state where the device has no
Settings app.

### Or ship a folder, with its own icon

An app can also be a **folder** — `apps/<name>/main.lua` — so it can carry its
own launcher icon and other assets. `tools/push.py apps/<name>` pushes the whole
folder and builds the icon; the launcher lists it by the folder name. Its code,
its icon, and its saved state then all live on the card together. See
**[SD_CARD_APPS.md](SD_CARD_APPS.md)** for the layout, the icon pipeline, and
installing/uninstalling. Everything in *this* document applies unchanged whether
your app is one file or a folder.

---

## Lua version, the standard library, and the trust model

Apps run on **Lua 5.5** — not 5.1, not LuaJIT. If your muscle memory is from either of
those, two things are likely to surprise you: `/` always returns a float even for two
integers (use `//` for integer division), and `math.random` is **seeded for you** from the
chip's hardware RNG at every app launch — you do not need `math.randomseed()`, and you get
a different sequence every run.
`math.random(n)` returns 1..n inclusive, `math.random(a, b)` returns a..b inclusive, and
`math.random()` returns a float in [0,1).

Available standard library: `string`, `math`, `table`, `os`, `io`, `utf8`, `coroutine`.

`require` gives you the launcher's modules. **This is the complete roster** — there are no
others, and you cannot `require` a `.lua` file of your own (see the trust model below):

| Module | What it is |
| --- | --- |
| `lvgl` | Widgets, screens, styles, fonts, symbols. Every app needs it. |
| `timer` | `every` / `after` / `now_ms`. 16 slots. |
| `ui` | Shared primitives — header, row, select, picker, confirm, toast, … |
| `button` | The PWR button |
| `keyboard` | Text entry |
| `store` | Per-app persistent key/value, saved on the card |
| `prefs` | Device settings (NVS) — survive a missing card, shared with the shell |
| `voice` | Offline speech — gate on `voice.available()` |
| `audio` | Tones and beeps |
| `rtc` | Wall-clock date and time |
| `imu` | Accelerometer, gyroscope, die temperature |
| `battery` | Charge percentage and charging state |
| `wifi` | Station-mode networking and NTP |

`rtc`, `imu`, `battery`, `wifi` and `voice` **degrade rather than raise**: when the
hardware or the data isn't there they return `nil, "reason"`. Always check the first
return.

**There is no sandbox.** An app has the full standard library and can read or write
anything reachable through it, including the SD card's other apps and anything else stored
there. `debug`, `package`, `os.exit`, and `os.execute` are removed — but that is not a
containment measure, it's self-defense for the *launcher*: those specific calls can disable
its watchdog hook, exit its process, or spawn a subprocess out from under it. Nothing stops
an app from opening and overwriting `/sdcard/apps/someone_elses_app.lua` with a plain
`io.open`. Installing an app means giving it full access to the card: only run apps you'd
trust with your own board.

---

## API

Everything comes from `local lvgl = require("lvgl")`, except timers (`require("timer")`).

### Widgets

All constructors take `(parent, opts)` and return an object:

```lua
local btn = lvgl.button(scr, { text = "OK", w = 240, h = 120, align = "center" })
```

Commonly useful ones:

| Constructor | Notes |
| --- | --- |
| `lvgl.label(p, {text=...})` | Text |
| `lvgl.button(p, {text=...})` | Remember the ≥200×100 sizing rule |
| `lvgl.slider(p, {min=,max=,value=})` | |
| `lvgl.arc(p, {min=,max=,value=})` | Circular control — suits this round-ish panel |
| `lvgl.bar(p, {min=,max=,value=})` | Progress |
| `lvgl.switch(p, {checked=})` · `lvgl.checkbox(p, {text=,checked=})` | |
| `lvgl.dropdown(p, {options={...}, selected=1})` · `lvgl.roller(...)` | **1-based** indexes |
| `lvgl.chart(p, {type="line", point_count=10, min=, max=})` | |
| `lvgl.led(p, {color="#00ff00", brightness=180, on=true})` | |
| `lvgl.spinner(p, {anim_ms=1000})` | |
| `lvgl.textarea(p, {...})` · `lvgl.keyboard(p, {textarea=ta})` | |
| `lvgl.tileview(p, {})` | Swipeable pages — see Paging, below |
| `lvgl.table`, `lvgl.list`, `lvgl.menu`, `lvgl.msgbox`, `lvgl.tabview`, `lvgl.calendar`, `lvgl.canvas`, `lvgl.line`, `lvgl.image`, `lvgl.spinbox`, `lvgl.buttonmatrix` | Also available |

### Common options

Accepted by most widgets:

- **Position and size**: `x`, `y`, `w`, `h`, `align`
- **Text**: `text`
- **Values**: `min`, `max`, `value`
- **Style**: `bg_color`, `text_color`, `border_color`, `bg_opa`, `opa`, `radius`,
  `border_width`, `pad`, `line_color`, `line_width`, `arc_width`, `font`
- **Slider/arc knob**: `knob_color`, `knob_pad` (grows the handle beyond the
  track, for a thin-track/big-knob slider), `knob_radius` (a large value rounds
  it to a circle)

Colors are strings or numbers: `bg_color = "#2f80ed"` or `text_color = 0xffffff`. `font`
takes a font object — see Fonts, below.

Align names: `center`, `top_mid`, `bottom_mid`, `left_mid`, `right_mid`, `top_left`,
`top_right`, `bottom_left`, `bottom_right`.

### Methods on any object

```lua
obj:set_pos(x, y)      obj:get_pos()    -- -> x, y
obj:set_size(w, h)     obj:get_size()   -- -> w, h
obj:align(name, x, y)
obj:set_style({ bg_color = "#1e1e28", radius = 12 })
obj:set_flex({ ... })  obj:set_grid({ ... })  obj:set_scroll({ ... })
obj:delete()           obj:clean()
```

**Text-holding widgets** (`label`, `button`, `checkbox`, `textarea`, …) have
`obj:set_text(s)`. This is the workhorse — it is how a timer callback updates a readout:

```lua
readout:set_text(string.format("%02d:%02d", h, m))
```

**Value-holding widgets** (`slider`, `bar`, `arc`, `spinbox`) have `obj:get_value()`,
`obj:set_value(n)` and `obj:set_range(min, max)`. `get_value()` is how you read state from
inside a `value_changed` callback, since callbacks take no arguments (see Events, below);
`set_value()` is how a timer or a button drives a bar or a gauge.

### Events

```lua
local handle = btn:on("clicked", function()
    print("tapped")          -- print goes to the serial console
end)

btn:off(handle)              -- or btn:off("clicked"), or btn:off()
```

Events: `clicked`, `pressed`, `released`, `long_pressed`, `value_changed`, `focused`,
`defocused`, `ready`, `cancel`, `gesture`.

**Swipes** arrive as the `gesture` event, and it has two rules of its own:

- **Register it on your screen object**, not a widget — LVGL delivers gestures to the
  screen, so `widget:on("gesture", ...)` raises rather than silently never firing.
- **A touch that scrolls something never produces a gesture.** Swiping a scrollable list
  or between tileview pages is scrolling; watch the tileview's `value_changed` for page
  changes instead. Gestures are for non-scrolling screens — games, canvases, dashboards.

Inside a gesture callback, `lvgl.gesture_dir()` returns `"left"`, `"right"`, `"top"` or
`"bottom"` (and `nil` anywhere else). Two swipes before your callback runs coalesce to
one — the latest direction wins.

```lua
scr:on("gesture", function()
    print("swiped " .. lvgl.gesture_dir())
end)
```

Callbacks take **no arguments** — read state from the widget itself, e.g.
`slider:get_value()` inside a `value_changed` handler, not a function argument. An error
inside a callback is logged (tag `lua_lvgl_evt`) and dispatch continues; it does not stop
your app or show the error screen.

### Timers

```lua
local timer = require("timer")

local h = timer.every(1000, function()
    print("tick")
end)

timer.after(250, function()
    print("fires once, 250ms later")
end)

h:cancel()
```

`timer.every(ms, fn)` repeats every `ms` milliseconds; `timer.after(ms, fn)` fires once.
Both return a handle with `:cancel()`. **You get 16 timer slots**; a 17th raises
`too many timers (max 16)`. Cancelled timers free their slot, and some `ui` helpers borrow
one while they're on screen (`ui.toast`). Every timer an app creates is cancelled
automatically when the app exits — you never need to track them down yourself.

#### `timer.every` keeps a schedule now. Read this if you saw the old advice.

**This section used to say `timer.every` was inaccurate and told you to work around it.
That was true, and it is no longer true.** The launcher re-armed a periodic timer from
the moment your callback *returned*, so every cycle silently absorbed the callback's
runtime plus dispatch latency and the error accumulated in one direction forever. It is
fixed: the deadline now advances by the period, so ticks sit on an absolute grid and a
late tick does not push the next one out.

Measured on hardware, before and after:

| | before | after |
| --- | --- | --- |
| `timer.every(1000, …)` | 5.0 ms/tick slow | **0.0 ms/tick** |
| `timer.every(100, …)` | 4.1 ms/tick slow | **0.0 ms/tick** |

(The figure this document carried for a long time — "roughly 24 ms per tick" — was stale
even before the fix.)

Two things follow, and the second one still bites:

**Counting ticks to measure elapsed time is now approximately right.** It was linearly
wrong before. `sim/timing_test.py` holds it to ±40 ms over a 4 s run and fails loudly if
anyone reintroduces the old re-arm.

**A slow callback still costs you ticks.** The grid does not create time. If your
callback takes longer than the period, the deadline is already in the past when it
returns and the launcher resynchronises to the next whole period rather than firing
repeatedly to catch up — so you *skip* ticks instead of drifting. A tick counter is
then an undercount, and no schedule can fix that. Which means:

**`timer.now_ms()` is still the right way to measure time, and the patterns below are
still the right way to write anything that must keep a beat.** They are robust whatever
the launcher does underneath, they cost nothing, and pattern 2 is what you need anyway
as soon as you pace against something other than a fixed period. What has changed is
the consequence of getting it wrong: an unnoticed steady drift before, a skipped tick
under load now.

`timer.now_ms()` returns monotonic milliseconds since boot, and it is the fix for all
three shapes of the problem:

**1. Measuring how long something took** — take two stamps and subtract. Never accumulate.

```lua
-- FRAGILE: accurate only while every callback finishes inside 10 ms.
-- Under load this undercounts, because missed ticks are skipped, not replayed.
local ms = 0
timer.every(10, function() ms = ms + 10 end)

-- RIGHT: true regardless of what the callbacks did
local started = timer.now_ms()
-- ...later...
local ms = timer.now_ms() - started
```

**2. Something that must happen on a beat** (a metronome, a countdown, an animation) —
chain `timer.after` against an absolute target, so a late tick cannot push the next one:

```lua
local next_at = timer.now_ms() + interval

local function schedule()
    local delay = math.floor(next_at - timer.now_ms())
    if delay < 1 then delay = 1 end          -- never schedule into the past
    timer.after(delay, function()
        beat()
        next_at = next_at + interval          -- absolute, so error cannot compound
        schedule()
    end)
end
```

Keep `next_at` a float if `interval` is fractional, but floor what you hand to
`timer.after` — it takes an integer and raises on a fraction.

**3. Watching something that changes on its own** (the RTC's seconds, a sensor) — do
**not** match its rate; sample *faster* and repaint only on change. Two independent
1 Hz clocks drift against each other no matter how accurate each one is, so a 1000 ms
timer sampling the RTC will eventually sit right on its second boundary and your display
jumps two at a time. Oversampling removes the problem instead of racing it:

```lua
local last
timer.every(250, function()
    local t = rtc.now()
    if not t or t.sec == last then return end
    last = t.sec
    -- repaint here; still only four times per second in the worst case
end)
```

Worked examples of all three: `apps/stopwatch.lua` and `apps/reaction.lua` (1),
`apps/metronome.lua` and `apps/countdown.lua` (2), and `apps/level.lua` (3).

**One limit on `timer.now_ms()`:** apps run with 32-bit Lua integers, so it wraps back to
negative after **2³¹ ms — about 24.9 days** of uptime. *Differences* survive that
(`now_ms() - started` stays correct across a single wrap, because Lua integer arithmetic
wraps too), which is exactly why the patterns above subtract rather than compare. Code
that instead assumes the value only ever grows will misbehave once, at that instant. Note
the simulator uses 64-bit Lua and never wraps, so it cannot show you this.

**The gotcha that costs real debugging time:** this looks reasonable and is wrong —

```lua
local h = timer.every(200, function() h:cancel() end)  -- BUG: never stops
```

A `local`'s scope begins *after* its own declaring statement finishes. The closure body is
compiled as part of `h`'s initializer, so inside it `h` resolves to a global — not the
local you're about to create. That global is `nil`, so `h:cancel()` throws every time the
timer fires; like any timer-callback error, that's logged and the timer keeps running
(errors do not cancel a timer). Net effect: it loops forever while quietly logging errors.
Declare the local first, assign second:

```lua
local h
h = timer.every(200, function() h:cancel() end)  -- correct: h is in scope when called
```

### Physical buttons

PWR — the bottom-right button — is yours. BOOT (top right) is Home and apps never see it.

```lua
local button = require("button")

local h = button.on("pwr", "pressed", function()
    print("PWR down")
end)

button.off(h)               -- or h:off()
button.is_down("pwr")       -- -> boolean
```

Events: `pressed`, `released`, `long_pressed` (fires once, after a 2 s hold). Callbacks
take **no arguments**, and an error inside one is logged and dispatch continues — same as
LVGL event callbacks. Every subscription is released automatically when your app exits.

The rules, adopted from Wear OS's multifunction-button guidance:

1. Use the button only for an **obvious binary action** (start/stop, play/pause, lap) or
   when the user isn't looking at the screen.
2. **Every button action must also be reachable from an on-screen control.** The button is
   an accelerator, never the only path.
3. One press, immediate. No multi-step or confirm-then-commit.
4. **Reversible only — never a destructive action.** Holding PWR ≥6 s powers the board
   off (hardware, below the launcher), so anything destructive sits one long press from
   data loss.

Latency is ~150 ms worst case (debounce + event pump) — fine for lap/pause, wrong for a
rhythm game. `button.on("boot", ...)` raises: Home is not interceptable, so a misbehaving
app is always escapable.

### Fonts

**Every widget defaults to Lexend 32** — the launcher sets it as the display theme, so you
get a readable size for free and usually don't need to touch fonts at all. **Eight** Lexend
faces ship baked into the firmware; ask for one with `lvgl.font`:

```lua
local big = lvgl.font(40)
label:set_style({ font = big })
```

| Size | Use |
| --- | --- |
| 24 | The floor. Never go below it. |
| 26 | Captions |
| 32 | Body — the theme default |
| 40, 48 | Headings |
| 60 | Hero numbers (stopwatch, score, temperature) |
| 72 | Top of the accessibility scale |
| 120 | Watch-face hero — **digits and `.:` only**, no letters |

Any other size raises. This is the whole list: nothing above 120 exists as a built-in, and
nothing between these sizes does either.

These cannot go missing — they live in flash, not on the SD card. Sizing guidance is in
`docs/DESIGN_GUIDE.md`: body 32, captions 26, never below 24.

**Text scales globally.** A user-set **font scale** (default **1.0**) drives both
`lvgl.font(size)` and the theme default, so everything sizes together.
You still request one of the eight sizes; the face returned is that size scaled, snapped to
the nearest available face. Read or set it with `lvgl.font_scale()`:

```lua
local s = lvgl.font_scale()      -- current scale, e.g. 0.8
lvgl.font_scale(1.0)             -- set 100% (clamped to 0.6–1.3); returns the new value
```

Apps rarely need this — the user changes it in **`apps/settings.lua`**, which saves the
choice; the launcher restores it at boot. Setting it re-sizes `lvgl.font()` text
immediately; theme-default text (plain labels) and the launcher pick the new
scale up when your app exits — the launcher re-applies the theme then. An app
that changes the scale without persisting does not affect anyone else: the
persisted value is restored on exit.

Icons come with them: `lvgl.symbol.*` holds the glyph strings. Concatenate them into label
text:

```lua
lvgl.label(scr, { text = lvgl.symbol.play .. " Start" })
```

They render at any `lvgl.font(size)`. **The complete set — 79 names, there are no
others:**

| Media | `.play` `.pause` `.stop` `.next` `.prev` `.shuffle` `.loop` `.volume_mid` `.volume_max` `.mute` `.audio` `.video` |
| Navigation | `.left` `.right` `.up` `.down` `.home` `.close` `.ok` `.plus` `.minus` `.refresh` `.list` `.bars` |
| Editing | `.edit` `.cut` `.copy` `.paste` `.save` `.trash` `.backspace` `.new_line` `.keyboard` `.search` |
| Status | `.battery_full` `.battery_3` `.battery_2` `.battery_1` `.battery_empty` `.charge` `.wifi` `.bluetooth` `.usb` `.gps` `.power` `.warning` `.bell` |
| Files | `.file` `.directory` `.download` `.upload` `.drive` `.sd_card` `.image` |
| Extended pack | `.clock` `.calendar` `.stopwatch` `.heart` `.heartbeat` `.star` `.sun` `.moon` `.thermometer` `.location` `.user` `.camera` `.fire` `.check_circle` `.comment` `.cloud` `.microphone` |
| Other | `.bullet` `.settings` `.tint` `.eject` `.eye_open` `.eye_close` `.call` `.envelope` |

Anything not on this list is `nil`, which concatenates to an error rather than a glyph.

Hero numbers use the built-in 60: `lvgl.font(60)`, or `lvgl.font(120)` for a watch face
where the time *is* the screen. A TTF from the card is only for a **typeface** we don't
compile in — not for size, which the built-ins already cover:

```lua
local font = lvgl.font_load("apps/big.ttf", { size = 64 })
label:set_style({ font = font })
```

The path is relative to the SD card root, so a TTF placed at `/sdcard/apps/big.ttf` (next
to your app is a fine place for it) loads as `"apps/big.ttf"`. **The file must actually
exist on the card** — `font_load` raises a Lua error if it's missing; it does not silently
fall back to the built-in font.

To make a font the *default* for every label an app creates (instead of setting `font` on
each one by hand), pass it to `lvgl.init`:

```lua
lvgl.init({ buffer_lines = 40, font_path = "apps/big.ttf", font_size = 64 })
```

`font_path` there resolves the same way, relative to the SD card root. If you skip all of
this, you get the theme default — Lexend 32 — which is the right answer for most apps.

### Keeping the screen on

The launcher dims the panel to 50% after 30 seconds of inactivity and blanks it
after 2 minutes, waking on the BOOT button. This is a power and heat measure,
not a style choice: on OLED the panel is the dominant load, and a board left lit
and idle measured ~68 °C — hot to the touch.

If the screen **is** your app — a watch face, a clock, a countdown someone
glances at — stop it blanking:

```lua
lvgl.keep_awake(true)     -- never blanks; STILL dims to 50% after 30s
lvgl.keep_awake(false)    -- back to normal
lvgl.keep_awake()         -- read the current state
```

**It suppresses the blank, not the dim.** A watch face at 50% is still a watch
face; one held at full brightness forever is the always-lit idle state this
exists to remove. Released automatically when your app exits — including if it
crashes — so you never need to clear it.

Reach for it when the screen **is** the feature. `flashlight.lua` uses the panel
as a lamp and `sign.lua` turns the watch into a held-up message; both are simply
broken if the screen blanks under them. (The watch faces used to be the worked
example here — they are part of the shell now, in C, and the shell's own
timeout governs them directly.)

Do **not** reach for it just because your app redraws. `metronome.lua` and
`countdown.lua` both deliberately skip it: their alarms are audible, so a dark
screen loses nothing, and both can run for tens of minutes.

While the screen is fully asleep, a finger on it does nothing — you cannot press
buttons you cannot see. **BOOT is the way back**, and a BOOT press on a dark
screen only wakes it; it does not exit your app. Press it again to leave.

### Shared UI: `require("ui")`

The building blocks every watch app needs, with the design guide's sizes baked
in — use these before hand-rolling. All of them ship in the launcher (apps
cannot `require` files from the card).

| Helper | What it gives you |
| --- | --- |
| `ui.title(scr, text)` | Page title, one consistent baseline across pages |
| `ui.header(scr, {title=, on_back=, kind=, action=, on_action=})` | Top-left back control + title. `kind="sheet"` → `×`; default → `‹`. **Omit `on_back` on root screens** — they get no back control (BOOT is the exit) |
| `ui.corner_button(scr, {text=, x=, y=, align=, w=, on_click=})` | Corner control: watch-scale visual, full 88px hit area. Icon-only → circle (omit `w`); text → pill (pass `w`) |
| `ui.row(parent, {text=, kind="toggle"\|"check"\|"nav", ...})` | Full-width 104px settings row; for toggles the whole row toggles |
| `ui.select(parent, {options=, selected=, disabled=}, cb)` | Single-select ✓-rows; owns the one-checked invariant; disabled rows acknowledge taps |
| `ui.picker({title=, options=, selected=, disabled=}, cb)` | Full-page dropdown replacement on its own screen; `cb(i)` or `cb(nil)` |
| `ui.confirm({title=, message=, confirm_label=, destructive=}, cb)` | **The only sanctioned path to a destructive action.** Full-width Cancel, 400ms arm delay on the confirm button |
| `ui.dots(scr, tv, {count=})` | Page dots for a tileview, bottom centre |
| `ui.toast(scr, text[, ms])` | Transient pill, 2.5s default (uses one of your 16 timer slots while up) |
| `ui.stepper(parent, {min=, max=, step=, value=, label=}, cb)` | +/- value row with clamp and hold-to-repeat |
| `ui.busy({text=})` | Modal spinner screen; call `h.done()` to dismiss |
| `ui.fill({title=, min=, max=, value=, label=}, on_change, done)` | Big drag-to-set arc **on its own screen** — a drag surface and horizontal paging fight over the same gesture, so never embed one in a tileview |
| `ui.button(parent, {text=, kind="primary"\|"secondary"\|"danger", w=, h=, align=, on_click=})` | The standard action button at the ≥200×100 tap size and launcher palette; returns the widget so `:on()`/`:set_text()` compose |
| `ui.list(parent, {y=, h=, pad_row=})` | Scrollable vertical stack — the container `ui.row`/`ui.select` expect; parent your rows to it |
| `ui.card(parent, {w=, h=, align=, x=, y=})` | Rounded grouped-content panel; parent content to it |
| `ui.stat(parent, {value=, label=, size=, align=, y=})` | Big value over a small caption (readouts); returns `h` with `h.set(v)` |
| `ui.note(scr, text, {size=, y=})` | Centred dim message for empty states and hints; returns the label |


**What each one hands back.** Most helpers return a handle you keep, so you can update the
thing later — this is how you change a row's label or read a stepper's value.

| Helper | Returns |
| --- | --- |
| `ui.corner_button` | `{ button, visual }` — `button` is the invisible ≥88 px tap target, `visual` the smaller glyph you see. Style `visual`; bind on `button`. |
| `ui.header` | `{ back, title, action }` — each present only if you asked for it |
| `ui.row` | `{ row, label, switch?, check?, get(), set_checked(on) }` |
| `ui.select` | `{ rows, selected, get(), set(i) }` |
| `ui.stepper` | `{ row, label, get(), set(v) }` |
| `ui.stat` | `{ value, caption?, set(v) }` |
| `ui.dots` | `{ labels }` |
| `ui.busy` | `{ done() }` — **you must call `done()`**, nothing else dismisses it |
| `ui.toast` | The pill widget; it removes itself |
| `ui.title` · `ui.note` | The label |
| `ui.button` | The button |
| `ui.list` · `ui.card` | The container — add children to it |
| `ui.confirm` · `ui.picker` · `ui.fill` | Nothing; they own a screen and call your callback |

### Text entry: `require("keyboard")`

Never hand-roll a QWERTY — its ~30px keys are unusable on this digitizer.

```lua
local keyboard = require("keyboard")
keyboard.open({ title = "Name", mode = "text", initial = current }, function(t)
    -- t == nil means cancelled; keep your old value then
end)
```

Letters are two-stage: tap a group (`ABCDEF`), then the letter; the view stays
in the group for repeats, `‹` goes back. `123` is a phone dialer. `Aa` toggles
case (auto-downshifts after the first letter and after spaces). Backspace is
always bottom-right; hold it to repeat. `×` asks before discarding non-empty
text. A voice key appears when `voice` is available (below).

`mode = "number"` opens straight to the dialer.

### Voice: `require("voice")`

Offline speech commands — MultiNet on the device, no network, no wake word.
Push-to-talk: capture runs when your app asks.

```lua
local voice = require("voice")

voice.available()   -- false if the model or mic is absent; degrade, don't die

-- One-shot vocabulary match: cb(word) or cb(nil) on timeout/no-match
voice.listen({ commands = { "start workout", "stop workout", "next set" } },
    function(word) ... end)

-- NATO spelling: say "romeo india charlie kilo", then "over".
-- "delete that" removes one letter. cb(text) or cb(nil).
voice.spell(function(text) ... end)

voice.stop()        -- cancel an active capture
```

Rules that come from hardware testing, not taste:

- **Commands want 2+ syllables.** "start"/"stop" verify; "lap" does not —
  use "lap time". Very short words fall below the detection threshold.
- Voice is an **accelerator**: every voice path needs a touch equivalent.
- One capture at a time — a second `listen`/`spell` returns `nil, "busy"`.
- A capture eats most of one core while running; keep them short and
  user-initiated.

### Sensors: `rtc`, `imu`, `battery`

Three chips on the board, each its own module. **All of them degrade
instead of raising** — a missing or unhappy sensor returns `nil` plus a
message, so an app still runs on a board where one is dead:

```lua
local t, err = rtc.now()
if not t then print("no clock: " .. err) end
```

**`rtc`** — the wall clock (survives reboots; battery-backed):

```lua
local rtc = require("rtc")
local t = rtc.now()          -- {year, month, day, hour, min, sec, wday} or nil, err
rtc.set{ year = 2026, month = 8, day = 22, hour = 14, min = 30, sec = 0, wday = 6 }
```

A clock that has never been set returns `nil, "rtc not set"` rather than a
plausible-looking wrong time — the chip flags its own loss of integrity, and
`rtc.set` is what clears it. **If your app shows time, handle that nil**: on a
fresh board it is the normal state until someone sets the clock.

**`imu`** — the 6-axis motion sensor:

```lua
local imu = require("imu")
local ax, ay, az = imu.accel()      -- g, one axis reads ~±1 at rest
local gx, gy, gz = imu.gyro()       -- UNCALIBRATED, see below
local c = imu.die_temp()            -- see the warning below
```

Poll it from a `timer.every`, not a loop. At rest the acceleration vector has
magnitude 1 g in whatever orientation the board is sitting — that is the check
to use if you suspect your maths. The accelerometer is good to roughly ±10%
(its axes disagree with each other by about that much), which is plenty for
tilt, orientation, shake and step detection, and not enough for precise
measurement.

**`imu.gyro()` is not calibrated — treat it as a motion detector, not an angle
source.** Measured on hardware it has a **7–9 °/s resting bias that drifts**
(the board accumulated ~12° while sitting perfectly still), which is enough on
its own to make integrated angles meaningless. Its **scale is correct** —
±512 °/s, 64 LSB/°/s, from the chip's `CTRL3` setting — so the *rate* it reports
is trustworthy; it is the bias, not the scaling, that makes integration useless.
Use it for "is the board turning, and roughly how fast"; do **not** integrate it
into degrees. If you need orientation, derive it
from `imu.accel()` — gravity is a reliable reference and was measured good.

`imu.die_temp()` is the **sensor's own silicon temperature, not the room**.
Measured against an 18.3 °C room it read 7.6 °C high, because it sits on a
powered board. It is useful for spotting thermal drift; it is not a
thermometer.

**`battery`**:

```lua
local battery = require("battery")
battery.percent()    -- 0-100, or nil, "gauge not ready"
battery.volts()      -- e.g. 4.14
battery.charging()   -- true while charging
battery.external()   -- true when USB power is present
```

### Networking: `require("wifi")`

Station mode only, and **nothing blocks** — connecting takes seconds, so
`connect()` starts the attempt and returns while your app keeps running:

```lua
local wifi = require("wifi")

wifi.connect()                  -- use the saved network
wifi.connect(ssid, password)    -- use these, and save them for next boot
wifi.status()                   -- "off" | "connecting" | "connected"
                                --   | "retrying" | "failed"
wifi.error()                    -- why it failed or is retrying, or nil
wifi.ip()                       -- "192.168.1.42", or nil
wifi.scan_start()               -- begin an async scan
wifi.scan_results()             -- nil while scanning; else a list of
                                --   { ssid=, rssi=, secure= }
wifi.time_synced()              -- true once NTP has set the clock this boot
wifi.disconnect() / wifi.forget()
```

**`"retrying"` is new, and it changes what `"failed"` means.** The board used to
give up permanently after five attempts. Now a **wrong password** still gives up
— retrying it would only keep the radio busy — but an **absent network** backs
off (30s → 5 min) and keeps trying, so the board reconnects on its own after a
router reboot or a walk back into range.

If you wrote `if wifi.status() == "failed"`, that branch no longer fires when the
network is simply out of range; it reports `"retrying"` instead. Test both, or
test `wifi.error() ~= nil`.

**Scanning is polled, like `status()`:**

```lua
wifi.scan_start()

timer.every(250, function()
    local nets = wifi.scan_results()
    if not nets then return end          -- still scanning
    for _, n in ipairs(nets) do
        print(n.ssid, n.rssi, n.secure)  -- strongest first, deduped
    end
end)
```

Results are sorted strongest-first, deduped by name (a dual-band AP or repeater
appears once, not three times), and hidden networks are omitted — offer manual
entry for those. `scan_results()` does not consume: reading twice gives the same
answer until the next `scan_start()`.

**A scan briefly interrupts an active connection.** That is accepted so you can
switch networks without disconnecting first. `scan_start()` returns
`nil, "connecting"` if a connection attempt is already in flight.

**`apps/settings.lua`'s Wi-Fi page is the worked example for all of this** — it
absorbed the old `apps/wifi_setup.lua`, so that is where the scan-then-pick flow,
the manual-entry fallback for hidden networks, and the retry states now live.

Poll `status()` from a `timer.every`, the same way you would poll anything
else. `"failed"` means it gave up after five attempts — usually a wrong
password.

**The clock sets itself.** When a connection comes up the launcher syncs time
over NTP and writes it to the RTC, so `rtc.now()` is correct after a reboot
with nobody typing a date. This is the intended way to set the clock;
`rtc.set` is the manual fallback.

Credentials are entered **on the device** in `apps/settings.lua` (Wi-Fi) and stored
in NVS by `wifi.connect(ssid, password)` itself, so a board with no card still
remembers its network. Do not ask a user to type a password into a host terminal.

**Your app cannot read them.** They live in a private NVS namespace that
`prefs` does not expose, and `prefs.get("wifi_pass")` raises rather than
answering. They used to sit in the shared `shell` namespace, where any app
could read the plaintext password in one line; a board upgrading from such a
build moves them out automatically on first boot. A network saved by an even
older build from `/sdcard/wifi.txt` is still imported once, the same way.

This is not a sandbox and does not pretend to be one — apps share the card and
can read each other's files, as said above. It is specifically the user's
network password, which is not your app's business.

**Captive portals do not work.** Hotel, café and conference networks intercept
traffic until you authenticate in a browser, and the device has no browser. The
symptom is legible rather than mysterious: `status()` reports `"connected"` and
`ip()` returns an address, but `time_synced()` stays false forever and no
request succeeds. If your app needs the network, say so when
`time_synced()` never becomes true rather than hanging on a request. A phone
hotspot is the usual workaround.

### Sound: `require("audio")`

Synthesised tones, not sample files — a metronome tick, a countdown
alarm and game feedback all need a beep, and a beep needs no assets on
the card. Nothing blocks: `tone` queues and returns.

```lua
local audio = require("audio")

audio.tone(880, 120)        -- Hz (0-8000), milliseconds (1-5000)
audio.beep()                -- a short click
audio.play{ {523,150}, {659,150}, {784,250} }   -- up to 16 notes; freq 0 is a rest
audio.volume(70)            -- 0-100; call with no argument to read it
audio.stop()
```

Notes are ramped in and out over 5 ms, because a square-edged start is a
step function and a step pops audibly whatever the tone.

**The speaker and the microphone share one I2S bus**, so playback and
`voice` are mutually exclusive: `voice.listen` returns
`nil, "audio playing"` while a tone is sounding. Keep tones short and
they will not collide.

The speaker opens on first use, never at boot — audio hardware that
misbehaves during startup would wedge the device before the launcher
exists, and that needs a physical button dance to recover.

### Persistence: `require("store")`

Remember things across runs — a high score, a settings choice, a list — without
hand-rolling files. Each app gets its own JSON file on the card (the launcher
points `store` at it); you never name the path.

```lua
local store = require("store")

local best = store.get("best", 0)     -- second arg is the default if never saved
if score > best then
    store.set("best", score)          -- in memory only
    store.save()                      -- THIS writes the card
end
```

| Call | What |
| --- | --- |
| `store.get(key, default)` | The saved value, or `default` if the key was never stored |
| `store.set(key, value)` | Set a value **in memory** (strings, numbers, booleans, and nested tables/arrays) |
| `store.save()` | Write the file. Returns `true`, or `nil, reason`. **Nothing persists until you call this** |
| `store.all()` | The whole state table, to read or mutate directly (then `save()`) |
| `store.clear()` | Forget everything (then `save()` to persist the empty state) |

`get`/`set` only touch memory, so a tight loop can `set` freely; call `save()`
once at a natural moment (game over, item added).

**If you forget `save()`, the launcher writes it for you when your app exits** —
including when the user presses BOOT mid-anything, and after a crash. BOOT lands
whenever it lands and there is no `on_exit` hook to save from, so "save after
every mutation" was a rule you could only follow perfectly or not at all. Call
`save()` at the natural moments anyway: it is the only way to survive a power
cut, and it is what makes the write happen *when you meant it*. What changed is
that forgetting is no longer silent data loss. Values must be JSON-friendly —
strings, numbers, booleans, and tables of those; a function or userdata will not
round-trip. The file is human-readable JSON, so you can inspect it on the card.

### Device settings: `require("prefs")`

`store` is for **your app's** state and lives on the card. `prefs` is for
**device** settings: small values in NVS that survive with no card in the slot,
and that the launcher's own C shell reads.

```lua
local prefs = require("prefs")

prefs.get("volume", 70)     -- the saved value, or the default
prefs.set("volume", 80)     -- written immediately; no save() step
prefs.clear("volume")       -- forget it
```

| Call | What |
| --- | --- |
| `prefs.get(key, default)` | The saved value, or `default` if never set |
| `prefs.set(key, value)` | Integers and strings. Commits straight away |
| `prefs.clear(key)` | Forget one key |

Keys are **1-15 characters** (an NVS limit) and values are integers or strings
— there is no table support, deliberately: this is for scalars, not documents.
A float is rounded to an integer, so `get` returns what `set` stored.

Most apps do not need this — it is how **`apps/settings.lua`** stores things the
shell must know about with no card present: `face` (the watch face style),
`tz_min` (minutes east of UTC), `tz_city` and `tz_dst` (which zone was picked
and whether summer time is on), `font_pct`, `volume`, and `fps` (the developer
overlay, below). Writing those keys from your own app changes the device's
settings, so treat them as the Settings app's.

`wifi_ssid` and `wifi_pass` are **not** among them: both names are refused by
`prefs.get` and `prefs.set`. See Networking, above.

The three timezone keys are one value in three parts, and the watch face reads
only `tz_min`:

```
tz_min == ui.ZONES[tz_city][2] + (tz_dst and 60 or 0)
```

So `tz_min` is the **effective** offset, already including summer time. Writing
it on its own leaves `tz_city`/`tz_dst` describing a different zone than the
face is showing — which is why this is Settings' job, not an app's.

### The FPS overlay

`lvgl.perf_overlay(true)` shows LVGL's frame-rate and CPU readout,
`lvgl.perf_overlay(false)` hides it, and calling it with no argument returns the
current state. It is **off by default** and lives in **Settings → Display &
sound**, which is where it belongs — like the other device-wide `prefs` keys,
it is the Settings app's control and not something an app should be reaching
for.

Two things about it that are easy to get wrong:

- **It draws on the display's *system layer*, not on your screen.** So it
  survives every screen swap on its own, including your app exiting — and
  `SHOT` can never capture it, because `lv_snapshot_take()` walks the active
  screen and the system layer is that screen's *sibling*. It is visible on the
  physical panel only.
- Because it survives your app, turning it on and leaving it on is exactly the
  intended use: you enable it in Settings, leave, and watch something else.

### More on widgets

A few methods and options that the widget table above does not show:

| Call | What |
| --- | --- |
| `obj:set_clickable(false)` | Make a widget display-only. **Use it on decorative controls in a paged view** — an interactive arc or slider swallows the horizontal drag and the page stops swiping |
| `line:set_points{ {x=,y=}, ... }` | Move a line after creation. Points are `{x=,y=}` tables — an array pair silently yields `(0,0)`, a zero-length line that is present, invisible, and raises nothing |
| `lvgl.line(p, { w=, h=, points= })` | Give a line an explicit size, or it shrinks to its points' bounding box and absolute coordinates collapse |
| `lvgl.arc(p, { bg_start_angle=, bg_end_angle=, rotation= })` | Arc geometry. Angles must be **0–360**; a full dial starting at twelve o'clock is `0, 360` with `rotation = 270` |
| `track_color` | An arc's *unlit* remainder. Without it the value and its background are the same colour, and 1% looks identical to 100% |
| `line_color` | The arc's *value* band (and its knob), or a line's colour |
| `obj:on("long_pressed_repeat", fn)` | Fires repeatedly while held — hold-to-repeat |
| `tv:get_active_index()` | A tileview's current page, 1-based |
| `lvgl.font(72)` · `lvgl.font(120)` | Larger faces. **120 is digits and `.:` only** — a full charset at that size costs 2 MB |

`ui` also exposes the timezone and calendar helpers the clock apps
share: `ui.zone()` returns city index, DST flag and offset in minutes;
`ui.shift(t, minutes)` shifts an `rtc.now()` reading and rolls the date
properly; `ui.DAYS` and `ui.MONTHS` are name tables.

### The data widgets, and how to fill them

The widget table lists these as "also available", which was not enough: you could
*create* a chart or a table and then had no documented way to put anything in it. Each
one's methods, in full.

**`lvgl.chart`** — series are handles you keep, not indexes:

```lua
local c = lvgl.chart(scr, { type = "line", point_count = 10, min = 0, max = 100 })
local s = c:add_series("#2F80ED")        -- optional 2nd arg: "primary_y" | "secondary_y"
c:set_series_values(s, { 10, 40, 25, 90 })
c:set_next_value(s, 55)                  -- push one point, scrolling the series
c:set_type("line")                       -- "line" | "bar" | "scatter"
c:set_point_count(20)  ·  c:set_range(0, 100, "primary_y")  ·  c:refresh()
```

**`lvgl.table`** — 1-based row and column:

```lua
t:set_cell(1, 1, "Mon")      t:get_cell(1, 1)   -- -> string
```

**`lvgl.list`** — both return the created child, so you can `:on("clicked", …)` it:

```lua
list:add_text("Section")                 -- a plain header row
local row = list:add_button(lvgl.symbol.file, "Open")   -- (icon, text); icon may be nil
```

**`lvgl.tabview`** — `add_tab` returns the tab's content container; parent your widgets to it:

```lua
local page = tv:add_tab("Stats")
tv:set_active(1)  ·  tv:get_active()  ·  tv:get_tab_count()  ·  tv:set_tab_text(1, "New")
```

**`lvgl.msgbox`** — each `add_*` returns the created child:

```lua
m:add_title("Delete?")  ·  m:add_text("This cannot be undone.")
m:add_footer_button("Cancel")  ·  m:add_close_button()  ·  m:close()
```

**`lvgl.spinbox`** — beyond the shared value methods:

```lua
sb:set_step(10)  ·  sb:get_step()  ·  sb:increment()  ·  sb:decrement()
sb:step_next()   ·  sb:step_prev()          -- move the edited digit
```

**`lvgl.led`**:

```lua
led:set_color("#00FF00")  ·  led:set_brightness(0..255)  ·  led:get_brightness()
led:on()  ·  led:off()  ·  led:toggle()
```

**`lvgl.buttonmatrix`** — `set_map` takes a flat list of strings; `"\n"` starts a new row:

```lua
bm:set_map({ "1", "2", "3", "\n", "4", "5", "6" })
bm:set_selected(i)  ·  bm:get_selected()  ·  bm:get_button_text(i)  ·  bm:set_one_checked(true)
```

**`lvgl.calendar`**:

```lua
cal:set_today(2026, 8, 22)  ·  cal:set_shown(2026, 8)
cal:set_highlighted({ {2026, 8, 22}, {2026, 8, 25} })   -- POSITIONAL {year, month, day}
cal:get_pressed_date()
```

Note the inconsistency, because it will bite: calendar dates are **positional arrays**
`{year, month, day}`, while `lvgl.line` points are **keyed tables** `{x=, y=}`. Passing the
wrong shape to `set_highlighted` raises; passing the wrong shape to `set_points` silently
draws nothing.

**`lvgl.canvas`** — pixel-level drawing:

```lua
cv:fill_bg("#000000", 255)   ·  cv:set_px(x, y, "#FF0000", 255)   ·  cv:get_px(x, y)
cv:set_rgb565_data(binary_string)
```

**`lvgl.window`** — `get_content()` is the parent for your widgets:

```lua
w:add_title("Log")  ·  w:add_button(lvgl.symbol.close, 40)
w:get_header()  ·  w:get_content()
```

`lvgl.menu` also has a full method set (`page`, `cont`, `section`, `separator`, `set_page`,
`set_sidebar_page`, `set_mode_header`, `set_root_back_button`, `clear_history`). It is
built for a sidebar-and-pages layout that does not suit a 368 px screen — prefer
`ui.picker` or a `tileview`.

### Layout

Three layout calls, on any object. **Both flex and grid are available** — grid is the
better tool whenever things must line up in columns, which flex only fakes.

**Flex** — a row or a column:

```lua
list:set_flex({
    flow = "column",     -- row · column · row_wrap · column_wrap
                         --   row_reverse · row_wrap_reverse
    main = "start",      -- along the flow:   start · end · center
    cross = "center",    -- across the flow:    space_evenly · space_around
    track = "start",     --   between wrapped tracks:  space_between
    pad_row = 16,        -- gap between rows
    pad_column = 8,      -- gap between columns
})
```

**Grid** — tracks are sizes in pixels, `"content"` (fit the child), or `"fr"` (share the
leftover space). Place each child yourself; `col`/`row` are **1-based**:

```lua
panel:set_grid({
    cols = { "fr", "fr" },        -- two equal columns
    rows = { 104, 104, "content" },
    col_align = "stretch",        -- start · end · center · stretch
    row_align = "start",          --   space_evenly · space_around · space_between
    pad_row = 16, pad_column = 12,
})

label:set_grid_cell({ col = 1, row = 1 })
wide:set_grid_cell({ col = 1, row = 3, col_span = 2 })   -- spans both columns
```

**Scroll** — only the fields you pass are applied, so you can change one without
disturbing the others:

```lua
list:set_scroll({
    dir = "ver",            -- hor · ver · all · none
    scrollbar = "auto",     -- off · on · auto · active
    snap_y = "center",      -- none · start · end · center
})
```

That last point matters: an earlier version applied *every* field including the ones you
omitted, so `set_scroll({ scrollbar = "off" })` on a tileview silently wiped its
scroll-snap and pages stopped snapping into place.

### Paging

Multi-page apps swipe horizontally between sibling pages — the watch pattern (a workout
app is metrics ↔ controls ↔ settings). `lvgl.tileview` does this out of the box:

```lua
local tv = lvgl.tileview(scr, {})
local page1 = tv:add_tile(1, 1, "right")   -- col, row are 1-based;
local page2 = tv:add_tile(2, 1, "hor")     -- dir = which way you can LEAVE
local page3 = tv:add_tile(3, 1, "left")    --   ("left"/"right"/"hor"/"ver"/"all")

tv:on("value_changed", function()          -- fires on every page change
    -- tv:get_active_tile() returns the current tile
end)
```

Build each page's widgets with the tile as their parent. Page changes are scrolling, so
they arrive as `value_changed` — not `gesture` (see Events).

### The active screen

`lvgl.active_screen()` returns a handle to whatever screen is currently loaded — useful
when something opens its own screen and needs to restore yours afterwards:

```lua
local caller = lvgl.active_screen()
-- ... show another screen ...
caller:load()
```

---

## Not available yet

Ask if your app genuinely needs one — each is launcher work that blocks everyone.
(Persistent storage used to live here; it now ships as `require("store")` — see
**Persistence**, below.)

---

## Limits

| Limit | Value |
| --- | --- |
| Display | **368 × 448** portrait, ~350 nit |
| Touch target | Aim **200 × 100**; hard floor **88 × 88** |
| Lua heap | Allocated from PSRAM (~5 MB free — the resident voice model costs ~3 MB). A bare VM (no modules loaded) costs ~15.5 KB; a real app — `lvgl` loaded, a screen created — costs ~40 KB |
| Lua task stack | 32 KB |
| Concurrency | One app at a time |
| Timers | **16** concurrent slots per app; a 17th raises |
| Fonts | Built-in Lexend at 24/26/32/40/48/60/72/120 via `lvgl.font(size)` (120 = digits and `.:` only). `lvgl.font_load` is for a different *typeface*, and the TTF must exist on the card |

---

## Worked example

The full contents of `apps/counter.lua`:

```lua
-- Counter -- the template app. Copy this file, rename it, make it yours.
--
-- Install: ./.venv/bin/python tools/push.py apps/counter.lua
-- Or copy it to /apps/ on the SD card. The launcher lists every .lua file it
-- finds there; the filename becomes the name shown in the list.

local lvgl = require("lvgl")
local ui = require("ui")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

local title = ui.title(scr, "Counter")

local count = 0

-- ui.button already carries the right tap size and the launcher palette.
-- Reach for a ui helper before hand-rolling a widget: touch on this panel is
-- not pixel-accurate, and small targets get missed. Measured, not guessed.
ui.button(scr, {
    text = "Tap me",
    align = "center", y = 0,
    on_click = function()
        count = count + 1
        title:set_text("Count: " .. count)
    end,
})

ui.note(scr, "edit apps/counter.lua", { y = 150, size = 26 })

scr:load()

-- Return and let the launcher pump events. Do NOT write your own
-- `while true` loop: it will freeze the device, including the way back.
```

For an example that combines wall-clock timing (`timer.now_ms`), the hero font,
and the PWR button used correctly, see `apps/stopwatch.lua`. (`font_load` has no
shipped example — the built-in sizes cover every app so far.)

---

## Debugging

`print()` goes to the serial console. **Reading that console live means
resetting the board**, which destroys the state you were trying to inspect --
so the launcher keeps the last 32 KB of its own log in memory and `LOG` (or
`tools/drive.py log`) hands it back over the port the harness already owns.
Trigger the fault, then ask what happened. It captures your `print()` **and** `ESP_LOGx` --
including the launcher's `app '<name>' failed:` and its traceback. `print()`
reaches it because the launcher replaces the VM's `print` with one that tees;
it behaves exactly as Lua's does (`tostring` on each argument, tabs between,
newline at the end), so nothing changes for you. A crashing app also shows its error on-screen (rule
4), with the full traceback on serial too, prefixed `app '<name>' failed:`. Errors inside
an event callback are logged separately under the tag `lua_lvgl_evt` and don't stop the
app or show the error screen.

Iterate without touching the SD card or reflashing:

```bash
./.venv/bin/python tools/push.py apps/myapp.lua      # install over USB
```

The same serial link can launch and stop apps without touching the screen:

```
RUN myapp.lua        ->  RUN_OK myapp.lua   (or RUN_ERR bad_name|not_found|already_running)
STOP                 ->  STOP_OK            (or STOP_ERR not_running)
LIST                 ->  APP <name> per line, then LIST_OK <n>
DELETE myapp.lua     ->  DELETE_OK          (or DELETE_ERR
                         bad_name|not_found|builtin|delete_failed)
SHOT                 ->  screenshot of the live screen (see tools/screenshot.py)
TAP <x> <y>          ->  synthetic tap, same event pipeline as a finger
SWIPE x0 y0 x1 y1 [ms] -> synthetic swipe/drag (a long press is a
                         zero-distance swipe with a long duration)
BOOT                 ->  BOOT_OK -- one BOOT press, through the same handler
                         the physical button calls: app -> home, home -> app
                         list, list -> home
LOG                  ->  LOG_BEGIN <bytes>, the buffered console output
                         (ESP_LOG since boot, oldest first), LOG_END
MEM                  ->  MEM <psram_free> <internal_free> <largest_internal>
PING                 ->  PONG launcher <proto> lvgl <x.y.z>  (confirm the port
                         really is the launcher before driving it)
BRIGHT <pct>         ->  BRIGHT_OK <pct> err=<code>   (panel brightness 0-100;
                         mainly for testing the screen timeout without waiting
                         out its 30s/2min steps)
<anything else>      ->  ERR unknown_command <verb>  (so a typo answers at once
                         instead of timing out silently)
STATS                ->  heap low-water marks, per-task CPU and stack
                         headroom (tools/stats.py decodes it)
```

`tools/push.py --list` and `--delete name.lua` wrap LIST/DELETE. For driving
and seeing the UI without touching the device:

```bash
tools/drive.py run myapp.lua : sleep 1 : tap 184 224 : sleep 0.5 : shot out.png
```

Watch the console live:

```bash
idf.py -p $(printf '%s\n' /dev/cu.usbmodem* | head -1) monitor   # Ctrl-] to exit
```

or capture a window of it without holding the port open: `tools/read_serial.py <seconds>`.
