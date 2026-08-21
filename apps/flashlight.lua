-- Flashlight -- the whole screen is the light. Tap anywhere (or PWR) to
-- toggle; drag the slider to set the level. On an AMOLED the panel itself is
-- the lamp, so this is a genuinely useful watch app.
--
-- Install: ./.venv/bin/python tools/push.py apps/flashlight.lua

local lvgl = require("lvgl")
local button = require("button")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()

local on = true
local level = 255                    -- gray level when on (40..255)

local function gray(v) return string.format("#%02x%02x%02x", v, v, v) end

local hint = lvgl.label(scr, {
    text = "tap to toggle", align = "top_mid", y = 24,
})

local function apply()
    scr:set_style({ bg_color = on and gray(level) or "#000000" })
    -- Keep the hint legible against whichever background is showing.
    hint:set_style({ text_color = on and "#303030" or "#505050" })
end

-- Full-screen invisible button catches taps anywhere. Created before the
-- slider so the slider (added later, higher z-order) still gets its own taps.
local pad = lvgl.button(scr, { x = 0, y = 0, w = 368, h = 448, bg_opa = 0 })
pad:on("clicked", function()
    on = not on
    apply()
end)

local slider = lvgl.slider(scr, {
    min = 40, max = 255, value = level,
    align = "bottom_mid", y = -40, w = 300,
})
slider:on("value_changed", function()
    level = slider:get_value()
    on = true
    apply()
end)

-- PWR toggles too; the whole screen is the on-screen control.
button.on("pwr", "pressed", function()
    on = not on
    apply()
end)

apply()
scr:load()
