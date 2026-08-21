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
scr:set_style({ bg_color = "#101014" })

local label = lvgl.label(scr, {
    text = "Hello",
    align = "center",
    text_color = "#ffffff",
})

scr:load()
```

That is a complete, working app.

### The rules that matter

1. **Build your UI, then return.** Do **not** write a `while true` loop or call
   `lvgl.run()`. The launcher pumps the event loop for you. An app that loops forever
   freezes the device, including the way back to the launcher. Need something recurring?
   Use the `timer` module (below), not a loop.
2. **Make your own screen** with `lvgl.create_screen()` and `scr:load()`. Never reach for
   the launcher's screen. When your app exits, the launcher deletes your screen and every
   widget on it — you do not need to clean up widgets.
3. **Touch is not pixel-accurate.** Measured on this hardware: a 240×120 button catches
   every tap, while a 180×56 one dropped roughly half. **Keep tappable targets ≥ ~200×100.**
   If your buttons feel broken, this is almost certainly why.
4. **Errors are visible, not fatal.** A Lua error stops your app and shows a red,
   full-screen error with a traceback, then waits for PWR — or `STOP` over serial — before
   returning to the launcher. It does not vanish silently, and it does not reboot the
   board. This is different for an error *inside an event callback* (a `btn:on(...)`
   handler): that is caught, logged to serial under the tag `lua_lvgl_evt`, and your app
   keeps running — see Events, below.
5. **PWR returns you to the launcher — almost always instantly.** It is hardware: no app
   can consume or override it. Under the hood, PWR works by interrupting the Lua
   interpreter, which is fast but can't reach everywhere: a tight loop **inside a
   coroutine body, or inside a blocking C call**, is invisible to that interrupt. Those get
   caught by a watchdog instead, which reboots the *entire board* — not just your app —
   after about 10 seconds. Either way you get back to a working launcher, but the
   coroutine/C-call path is slower and noisier. Practical takeaway: never write a
   `while true` loop anywhere in your app, coroutine or not (rule 1), and use `timer` for
   anything periodic.

---

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
integers (use `//` for integer division), and `math.random`'s range and seeding rules are
different. Don't assume; test the arithmetic you care about.

Available standard library: `string`, `math`, `table`, `os`, `io`, `utf8`, `coroutine`.
`require` also works — you need it for `require("lvgl")` and `require("timer")`.

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

Value-holding widgets (`slider`, `bar`, `arc`, `spinbox`) also have `obj:get_value()` —
this is how you read state from inside a `value_changed` callback, since callbacks take no
arguments (see Events, below).

### Events

```lua
local handle = btn:on("clicked", function()
    print("tapped")          -- print goes to the serial console
end)

btn:off(handle)              -- or btn:off("clicked"), or btn:off()
```

Events: `clicked`, `pressed`, `released`, `long_pressed`, `value_changed`, `focused`,
`defocused`, `ready`, `cancel`.

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
Both return a handle with `:cancel()`. Every timer an app creates is cancelled
automatically when the app exits — you never need to track them down yourself.

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

### Fonts

Every label defaults to LVGL's built-in 14px font. For anything where the point is one big
number — a stopwatch, a score, a temperature — load a real TTF instead:

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

`font_path` there resolves the same way, relative to the SD card root. No font ships with
the launcher by default — if you skip both of these, you get the built-in 14px font.

### Layout

```lua
list:set_flex({ flow = "column", pad_row = 10 })
```

---

## Not available yet

Ask if your app genuinely needs one — each is launcher work that blocks everyone:

- Wi-Fi / networking
- Audio playback and the microphone
- IMU, RTC, and battery readings
- A dedicated persistent-storage API. There's no `app.store` — if you need to remember
  something across runs, use `io.open` on a file under `/sdcard` yourself (see the trust
  model above: nothing stops you, there's just no helper for it)

---

## Limits

| Limit | Value |
| --- | --- |
| Display | **368 × 448** portrait, ~350 nit |
| Minimum comfortable touch target | **~200 × 100** |
| Lua heap | Allocated from PSRAM (~8 MB free). A bare VM (no modules loaded) costs ~15.5 KB; a real app — `lvgl` loaded, a screen created — costs ~40 KB |
| Lua task stack | 32 KB |
| Concurrency | One app at a time |
| Custom fonts | Available — `lvgl.font_load(path, {size=...})` or `lvgl.init({font_path=...})`. The TTF must exist on the SD card; none ships by default |

---

## Worked example

The full contents of `apps/counter.lua`:

```lua
local lvgl = require("lvgl")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#101014" })

local title = lvgl.label(scr, {
    text = "Counter",
    align = "top_mid", y = 30,
    text_color = "#ffffff",
})

local count = 0

local button = lvgl.button(scr, {
    text = "Tap me",
    align = "center", y = 0,
    w = 240, h = 120,          -- big on purpose; see rule 3
    bg_color = "#2f80ed",
    text_color = "#ffffff",
})

button:on("clicked", function()
    count = count + 1
    title:set_text("Count: " .. count)
end)

scr:load()
```

For an example that combines a timer, a big custom font, and start/stop/reset buttons, see
`apps/stopwatch.lua`.

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
RUN myapp.lua   ->  RUN_OK myapp.lua   (or RUN_ERR bad_name|not_found|already_running)
STOP            ->  STOP_OK            (or STOP_ERR not_running)
```

Watch the console live:

```bash
idf.py -p /dev/cu.usbmodem101 monitor      # Ctrl-] to exit
```

or capture a window of it without holding the port open: `tools/read_serial.py <seconds>`.
