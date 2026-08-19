# App Contract

Everything an app author needs. If you are writing an app rather than the launcher, this
is the only document you need to read.

**Status: DRAFT — frozen at the API freeze deadline (early afternoon).** Until then,
raise gaps early; after then, additions wait for v2 so nobody's in-flight work breaks.

---

## The shape of an app

One Lua file at `/sdcard/apps/<name>.lua` that **returns a table**:

```lua
return {
  name = "Clock",                    -- shown in the launcher list
  icon = "clock.png",                -- optional, resolved next to this script

  start = function(root)             -- required: build your UI into `root`
  end,

  update = function(dt)              -- optional: called from the LVGL task
  end,

  stop = function()                  -- optional: release non-UI resources
  end,
}
```

That is the whole contract. Three optional hooks, one required.

### The rules that matter

1. **You get a container, not the screen.** `root` is an `lv_obj` sized to the display.
   Build everything inside it. The launcher deletes `root` when your app exits, so every
   widget you parent to it is cleaned up automatically — you do not need to free widgets
   in `stop()`.
2. **`stop()` is for everything that is not a widget** — open files, and any state you
   want persisted. Timers created via `app.timer` are cancelled for you.
3. **Never call `lv_scr_act()` or touch anything outside `root`.** That is the launcher's
   UI; reaching into it is how one app takes down everyone's demo.
4. **Do not block.** `start()` and `update()` run on the LVGL task. A `while true` loop
   or a long sleep freezes the whole device, including the way back to the launcher. Use
   `app.timer` for anything periodic.
5. **Errors are caught, not fatal.** Every hook runs inside a protected call; a Lua error
   shows on screen and returns to the launcher rather than rebooting the board. You will
   see the message and traceback on the serial console.

---

## API

Available as the global `app` inside your script.

### UI

Widgets are created as children of a parent object, starting from `root`:

```lua
local label  = app.label(root, "Hello")
local button = app.button(root, "Tap me")
local slider = app.slider(root, 0, 100)
local image  = app.image(root, "picture.png")
local arc    = app.arc(root, 0, 100)
```

Common methods on any object:

| Call | Effect |
| --- | --- |
| `obj:set_text(s)` | Set text (label, button) |
| `obj:set_pos(x, y)` | Position within the parent |
| `obj:set_size(w, h)` | Size in pixels |
| `obj:align(a, dx, dy)` | Align: `"center"`, `"top_mid"`, `"bottom_mid"`, `"left_mid"`, `"right_mid"` |
| `obj:center()` | Shorthand for `align("center", 0, 0)` |
| `obj:set_style(t)` | Table of style props, e.g. `{bg_color = 0x1E1E1E, radius = 12}` |
| `obj:delete()` | Remove early (rarely needed) |

### Events

```lua
button:on("clicked", function() print("tapped") end)
slider:on("value_changed", function(v) label:set_text(tostring(v)) end)
```

Events: `"clicked"`, `"pressed"`, `"released"`, `"value_changed"`, `"long_pressed"`.

### Timers

```lua
local t = app.timer(1000, function() ... end)   -- repeating, every 1000 ms
app.after(250, function() ... end)              -- one-shot
t:cancel()                                      -- all timers auto-cancel on exit
```

### Gestures

```lua
app.on_gesture(function(dir)     -- "up" | "down" | "left" | "right"
  if dir == "right" then app.exit() end
end)
```

Swipe-from-left-edge is **reserved by the launcher** as the universal "back" gesture.
Do not consume it.

### Storage

Per-app, sandboxed to your own directory. Survives reboots.

```lua
app.store:set("high_score", 42)
local best = app.store:get("high_score") or 0
```

Values may be strings, numbers, or booleans.

### Assets and paths

`app.path` is the directory your script lives in. Bundled assets resolve relative to it,
so `app.image(root, "picture.png")` finds `/sdcard/apps/picture.png`.

### Hardware

```lua
local x, y, z = app.imu.accel()      -- g
local gx, gy, gz = app.imu.gyro()    -- deg/s
local t = app.rtc.now()              -- {year, month, day, hour, min, sec, wday}
local pct, volts, charging = app.battery()
app.brightness(200)                  -- 0-255; restored on exit
```

### Lifecycle and logging

```lua
app.exit()                -- return to the launcher
print("anything")         -- goes to the serial console
```

---

## Not available on day one

Deliberately omitted to keep the launcher small — every entry here is work that blocks
five other people. Ask if your app genuinely needs one:

- Networking / Wi-Fi
- Audio playback and the microphone
- Filesystem access outside your app directory
- Threads or coroutines
- Raw pointers, `userdata`, FFI, or any escape hatch to C

---

## Limits

| Limit | Value |
| --- | --- |
| Display | **368 × 448** portrait, ~350 nit |
| Lua heap per app | Allocated from **PSRAM**, so it cannot starve the launcher's internal DRAM |
| Lua task stack | 32 KB |
| One app at a time | Launching a new app stops the current one |

---

## Worked example

A complete, working app — copy `apps/template/` and start here:

```lua
local count = 0

return {
  name = "Counter",

  start = function(root)
    root:set_style({bg_color = 0x101014})

    local label = app.label(root, "0")
    label:set_style({text_color = 0xFFFFFF})
    label:align("center", 0, -40)

    local button = app.button(root, "Count")
    button:align("center", 0, 40)
    button:on("clicked", function()
      count = count + 1
      label:set_text(tostring(count))
      app.store:set("count", count)
    end)

    count = app.store:get("count") or 0
    label:set_text(tostring(count))
  end,

  stop = function()
    app.store:set("count", count)
  end,
}
```

---

## Testing without a board

If boards are shared, you are not blocked. The Lua↔LVGL binding this launcher is built on
ships an **Emscripten browser simulator**, so app UI can be developed and reviewed in a
browser before it ever touches hardware. Ask the launcher team for the simulator build.
