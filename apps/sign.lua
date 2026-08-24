-- Sign -- turn the watch into a held-up message: type a word or two and it
-- fills the screen. Tap the corner to cycle the size. Handy for "BE RIGHT
-- BACK", your name at a badge table, or a number at pickup.
--
-- Install: ./.venv/bin/python tools/push.py apps/sign.lua

local lvgl = require("lvgl")
local ui = require("ui")
local keyboard = require("keyboard")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

local text = ""
-- Hero sizes: a held-up sign is read from across a room, so use the hero faces
-- (60/72), not body/heading. All letters, so 120 (digits-only) is out.
local sizes = { 40, 60, 72 }
local size_i = 3

-- No fixed width: the label shrinks to its text so align="center" truly
-- centres it. Short messages are the norm for a sign; a very long one clips
-- rather than wrapping, which is the right trade for a glanceable display.
local display = lvgl.label(scr, {
    text = "", align = "center", y = -40, text_color = "#ffffff",
})

local function refresh()
    display:set_text(text ~= "" and text or "tap Edit")
    display:set_style({
        font = lvgl.font(sizes[size_i]),
        text_color = text ~= "" and "#ffffff" or "#A0A0AE",
    })
end

local edit = lvgl.button(scr, {
    text = lvgl.symbol.edit .. " Edit",
    align = "bottom_mid", y = -28, w = 320, h = 108,
    bg_color = "#2f80ed", text_color = "#ffffff",
})
edit:on("clicked", function()
    keyboard.open({ title = "Sign", mode = "text", initial = text }, function(t)
        if t then
            text = t
            refresh()
        end
    end)
end)

-- Corner chip cycles the text size.
ui.corner_button(scr, {
    text = "A", align = "top_right", w = 100,
    on_click = function()
        size_i = size_i % #sizes + 1
        refresh()
    end,
})

refresh()
scr:load()
