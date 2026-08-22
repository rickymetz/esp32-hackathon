-- Hello World -- a second app, to prove the launcher lists more than one.
--
-- Install: copy to /apps/ on the SD card. The launcher lists every .lua file
-- it finds there; the filename becomes the name shown in the list.

local lvgl = require("lvgl")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

local title = lvgl.label(scr, {
    text = "Hello World",
    align = "top_mid", y = 30,
    text_color = "#ffffff",
})

local count = 0

-- Touch on this panel is not pixel-accurate: small targets get missed.
-- Keep tappable things at least ~200x100. This was measured, not guessed.
local button = lvgl.button(scr, {
    text = "Tap me",
    align = "center", y = 0,
    w = 240, h = 120,
    bg_color = "#27ae60",
    text_color = "#ffffff",
})

button:on("clicked", function()
    count = count + 1
    title:set_text("Count: " .. count)
end)

lvgl.label(scr, {
    text = "app two",
    align = "bottom_mid", y = -30,
    text_color = "#A0A0AE",
})

scr:load()

-- Return and let the launcher pump events. Do NOT write your own
-- `while true` loop: it will freeze the device, including the way back.
