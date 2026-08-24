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

-- One labelled slider per channel. A 26px track is a slim bar that's still an
-- easy drag (sliders are dragged, not tapped); channel letter, track and value
-- share one vertical centre (cy), and the track stops well short of the value
-- column so the knob can't overrun the number even at 255.
local SLIDER_H = 26
local function channel(i, cy, name, color)
    lvgl.label(scr, {
        text = name, align = "left_mid", x = 22, y = cy - 224,
        text_color = color, font = lvgl.font(32),
    })
    local val = lvgl.label(scr, {
        text = tostring(rgb[i]), align = "right_mid", x = -16, y = cy - 224,
        text_color = "#A0A0AE", font = lvgl.font(32),
    })
    local s = lvgl.slider(scr, {
        min = 0, max = 255, value = rgb[i],
        x = 56, y = cy - SLIDER_H // 2, w = 200, h = SLIDER_H,
    })
    s:on("value_changed", function()
        rgb[i] = s:get_value()
        val:set_text(tostring(rgb[i]))
        update()
    end)
end

-- cy is each channel's centreline; align="*_mid" places the labels there too
-- (y is the offset from the screen's vertical centre, 224).
channel(1, 250, "R", "#ff6b6b")
channel(2, 322, "G", "#51cf66")
channel(3, 394, "B", "#4dabf7")

update()
scr:load()
