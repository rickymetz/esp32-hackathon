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

`buffer_lines` sizes LVGL's render buffer, in screen rows. **Copy the 40 and move on** —
it is a memory-versus-redraw-smoothness trade, 40 suits every app here, and the only reason
to change it is a full-screen animation that visibly tears (raise it) on a screen that is
368 px wide.

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
5. **BOOT (the top-right button) returns you to the launcher — almost always instantly.**
   It is hardware: no app can consume or override it. (PWR, bottom right, belongs to your
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

# 4. Launch it -- either tap Refresh then the row on the device, or:
./.venv/bin/python tools/drive.py run myapp.lua
```

Edit, re-run steps 3 and 4, repeat. That is the whole loop.

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

This sends the file to the board over USB and rescans the app list. Tap **Refresh** on the
device, or send `RUN myapp.lua` over serial (see Debugging) to launch it immediately —
either way finds it right away, no reboot needed.

You can still do it by hand: copy the `.lua` file to `/apps/` on the microSD card. That is
the same folder as `/sdcard/apps` on the device — one name is how you see it when the card
is in a computer, the other is how the device's filesystem sees it, but there is only one
folder. The launcher scans it at boot and after every push, and lists every `.lua` file it
finds. **The filename becomes the name in the list**: `weather_clock.lua` shows as
"Weather clock".

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

#### `timer.every` is not accurate. Plan for that.

**A periodic timer re-arms *after* your callback returns.** So `timer.every(1000, …)`
does not fire every 1000 ms — it fires every 1000 ms *plus* however long your callback
took *plus* the pump's dispatch latency. That overhead was measured on this board at
roughly **24 ms per tick**, and it never averages out: it accumulates in one direction,
always slow.

This is the single most common bug in the apps written so far — five of the shipped
examples had it. It is invisible on screen, which is what makes it dangerous: the app
looks right and the numbers are wrong.

`timer.now_ms()` returns monotonic milliseconds since boot, and it is the fix for all
three shapes of the problem:

**1. Measuring how long something took** — take two stamps and subtract. Never accumulate.

```lua
-- WRONG: reports less time than actually passed
local ms = 0
timer.every(10, function() ms = ms + 10 end)

-- RIGHT
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
**not** match its rate; sample *faster* and repaint only on change. A 1000 ms timer
sampling a 1 Hz clock is slightly slower than the clock, so it misses whole seconds and
your display visibly jumps two at a time:

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
`apps/metronome.lua` and `apps/countdown.lua` (2), `apps/faces.lua` and `apps/clock.lua` (3).

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
its own to make integrated angles meaningless. Its scale is unverified — not
known wrong, just unchecked. Use it for "is the board turning, and roughly how
fast"; do **not** integrate it into degrees. If you need orientation, derive it
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
wifi.status()                   -- "off" | "connecting" | "connected" | "failed"
wifi.ip()                       -- "192.168.1.42", or nil
wifi.time_synced()              -- true once NTP has set the clock this boot
wifi.disconnect() / wifi.forget()
```

Poll `status()` from a `timer.every`, the same way you would poll anything
else. `"failed"` means it gave up after five attempts — usually a wrong
password.

**The clock sets itself.** When a connection comes up the launcher syncs time
over NTP and writes it to the RTC, so `rtc.now()` is correct after a reboot
with nobody typing a date. This is the intended way to set the clock;
`rtc.set` is the manual fallback.

Credentials are entered **on the device** with `apps/wifi_setup.lua` and stored
on the card. Do not ask a user to type a password into a host terminal.

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

### Layout

```lua
list:set_flex({ flow = "column", pad_row = 10 })
```

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

Ask if your app genuinely needs one — each is launcher work that blocks everyone:

- A dedicated persistent-storage API. There's no `app.store` — if you need to remember
  something across runs, use `io.open` on a file under `/sdcard` yourself (see the trust
  model above: nothing stops you, there's just no helper for it)

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

`print()` goes to the serial console. A crashing app also shows its error on-screen (rule
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
DELETE myapp.lua     ->  DELETE_OK          (or DELETE_ERR not_found|delete_failed)
SHOT                 ->  screenshot of the live screen (see tools/screenshot.py)
TAP <x> <y>          ->  synthetic tap, same event pipeline as a finger
SWIPE x0 y0 x1 y1 [ms] -> synthetic swipe/drag
```

`tools/push.py --list` and `--delete name.lua` wrap LIST/DELETE. For driving
and seeing the UI without touching the device:

```bash
tools/drive.py run myapp.lua : sleep 1 : tap 184 224 : sleep 0.5 : shot out.png
```

Watch the console live:

```bash
idf.py -p /dev/cu.usbmodem101 monitor      # Ctrl-] to exit
```

or capture a window of it without holding the port open: `tools/read_serial.py <seconds>`.
