-- Audio smoke test: prints results to serial so it needs no eyes, and
-- the tones can be heard from across a room.
local audio = require("audio")
local timer = require("timer")
local lvgl = require("lvgl")
local ui = require("ui")

lvgl.init({ buffer_lines = 40 })
local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })
ui.title(scr, "Audio")
local l = lvgl.label(scr, { text = "starting...", align = "center",
    text_color = "#ffffff" })

local ok, err = audio.tone(880, 200)
print("AUDIO_TONE " .. tostring(ok) .. " " .. tostring(err))
l:set_text(ok and "tone sent" or ("err: " .. tostring(err)))

timer.after(700, function()
    -- an ascending arpeggio, which also exercises the queue
    local ok2 = audio.play({ {523,150}, {659,150}, {784,150}, {1047,250} })
    print("AUDIO_PLAY " .. tostring(ok2))
end)

timer.after(2200, function()
    print("AUDIO_VOL " .. tostring(audio.volume()))
    print("AUDIO_DONE")
end)

scr:load()
