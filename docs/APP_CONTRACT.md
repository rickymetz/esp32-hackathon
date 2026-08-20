# App Contract

Everything you need to write an app. If you are writing an app rather than working on the
launcher, this is the only document you need.

**Verified against the running launcher** — every example here has been executed on the
board, not sketched.

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

### The five rules

1. **Build your UI, then return.** Do **not** write a `while true` loop or call
   `lvgl.run()`. The launcher pumps the event loop for you. An app that loops forever
   freezes the device, including the way back to the launcher.
2. **Make your own screen** with `lvgl.create_screen()` and `scr:load()`. Never reach for
   the launcher's screen. When your app exits, the launcher deletes your screen and every
   widget on it — you do not need to clean up widgets.
3. **Touch is not pixel-accurate.** Measured on this hardware: a 240×120 button catches
   every tap, while a 180×56 one dropped roughly half. **Keep tappable targets ≥ ~200×100.**
   If your buttons feel broken, this is almost certainly why.
4. **Errors are contained, not fatal.** A Lua error is logged to serial and returns you to
   the launcher rather than rebooting the board.
5. **The PWR button always returns to the launcher.** It is hardware — you cannot consume
   or override it, and users rely on it. (Holding it ≥6 s still powers the board off.)

---

## Installing your app

Copy the `.lua` file to `/apps/` on the microSD card. That is the whole process — no
reflash, no rebuild, no toolchain.

The launcher scans `/apps` at boot and lists every `.lua` file it finds. **The filename
becomes the name in the list**: `weather_clock.lua` shows as "Weather clock".

---

## API

Everything comes from `local lvgl = require("lvgl")`.

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

Colors are strings or numbers: `bg_color = "#2f80ed"` or `text_color = 0xffffff`.

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

### Events

```lua
local handle = btn:on("clicked", function()
    print("tapped")          -- print goes to the serial console
end)

btn:off(handle)              -- or btn:off("clicked"), or btn:off()
```

Events: `clicked`, `pressed`, `released`, `long_pressed`, `value_changed`, `focused`,
`defocused`, `ready`, `cancel`.

Callbacks take **no arguments** — read state from the widget itself. An error inside a
callback is logged and dispatch continues.

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
- Persistent key-value storage
- Writing files

---

## Limits

| Limit | Value |
| --- | --- |
| Display | **368 × 448** portrait, ~350 nit |
| Minimum comfortable touch target | **~200 × 100** |
| Lua heap | Allocated from PSRAM (~8 MB free); an app VM costs ~40 KB |
| Lua task stack | 32 KB |
| Concurrency | One app at a time |
| Custom fonts | The default TTF is absent, so LVGL's built-in font is used |

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

---

## Debugging

`print()` goes to the serial console. To watch it:

```bash
idf.py -p /dev/cu.usbmodem101 monitor      # Ctrl-] to exit
```

Lua errors appear there with a traceback, prefixed `app '<name>' failed:`.
