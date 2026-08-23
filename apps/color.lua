-- Color -- mix an RGB color with three sliders and read the hex. The swatch
-- and hex update live; the hex text flips to black on light colors so it stays
-- readable.
--
-- Install: ./.venv/bin/python tools/push.py apps/color.lua

local lvgl = require("lvgl")
local ui = require("ui")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

ui.title(scr, "Color")

local rgb = { 47, 128, 237 }         -- start on the device's signal blue

local swatch = lvgl.container(scr, {
    x = 24, y = 70, w = 320, h = 120, radius = 18, border_width = 0,
})
local hex = lvgl.label(swatch, { align = "center", text = "" })
hex:set_style({ font = lvgl.font(32) })

local function update()
    local col = string.format("#%02x%02x%02x", rgb[1], rgb[2], rgb[3])
    swatch:set_style({ bg_color = col })
    hex:set_text(col:upper())
    local lum = 0.299 * rgb[1] + 0.587 * rgb[2] + 0.114 * rgb[3]
    hex:set_style({ text_color = lum > 140 and "#000000" or "#ffffff" })
end

-- One labelled slider per channel. Sliders are 48px tall (default ~10px sits in
-- the band this digitizer drops ~half of) and 78px apart so a drag can't grab
-- the neighbouring channel; a live 0-255 readout sits on the right.
local function channel(i, y, name, color)
    lvgl.label(scr, { text = name, x = 22, y = y + 12, text_color = color })
    local val = lvgl.label(scr, {
        text = tostring(rgb[i]), align = "top_right", x = -18, y = y + 12,
        text_color = "#A0A0AE",
    })
    local s = lvgl.slider(scr, {
        min = 0, max = 255, value = rgb[i], x = 58, y = y, w = 232, h = 48,
    })
    s:on("value_changed", function()
        rgb[i] = s:get_value()
        val:set_text(tostring(rgb[i]))
        update()
    end)
end

channel(1, 210, "R", "#ff6b6b")
channel(2, 288, "G", "#51cf66")
channel(3, 366, "B", "#4dabf7")

update()
scr:load()
