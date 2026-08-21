-- voice module test: tap Listen, then say "start", "stop", or "lap".
local lvgl = require("lvgl")
local voice = require("voice")
local ui = require("ui")

lvgl.init({ buffer_lines = 40 })
local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

ui.title(scr, "Voice")

local status = lvgl.label(scr, {
    text = voice.available() and "model ready" or "unavailable",
    align = "center", y = -40,
    text_color = "#ffffff",
})

local btn = lvgl.button(scr, {
    text = "Listen",
    align = "bottom_mid", y = -16,
    w = 344, h = 104,
    bg_color = "#2F80ED", text_color = "#ffffff", radius = 12,
})

btn:on("clicked", function()
    status:set_text("listening...")
    local ok, err = voice.listen({ commands = { "start", "stop", "lap" } },
        function(word)
            status:set_text(word and ("heard: " .. word) or "timeout")
            print("VOICE_RESULT " .. tostring(word))
        end)
    if not ok then status:set_text("err: " .. tostring(err)) end
end)

scr:load()
