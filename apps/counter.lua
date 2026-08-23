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
