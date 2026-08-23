-- Tone -- a signal generator for the speaker.
--
-- Drag the slider to pick a frequency, tap Play (or press PWR) to sound it for
-- 400ms through audio.tone(). A pad of tiny piano keys would fight this
-- digitizer (the >=200x100 rule), so the control is one tall slider and one big
-- button -- which also matches what the audio module actually is: synthesised
-- tones, not samples. The slider spans the whole 100-2000 Hz range, so every
-- frequency (concert A, middle C, whatever) is one drag away.
--
-- audio is an accelerator, not the only path: Play is on-screen and PWR just
-- mirrors it (on release, so beginning a power-off hold doesn't beep). In the
-- simulator the speaker is silent (audio no-ops) but the whole UI and the
-- tone() calls run, so the app is verified board-free; the sound is confirmed
-- on hardware.

local lvgl = require("lvgl")
local audio = require("audio")
local button = require("button")
local ui = require("ui")

lvgl.init({ buffer_lines = 40 })

local FMIN, FMAX = 100, 2000
local freq = 440
local have_audio = audio.available()

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

ui.title(scr, "Tone")

local readout = lvgl.label(scr, {
    text = freq .. " Hz", align = "top_mid", y = 96,
    text_color = "#2F80ED", font = lvgl.font(60),
})

-- Tall enough to grab and drag on this digitizer (28px, the old height, is
-- below the ~56px that already dropped half its taps).
local slider = lvgl.slider(scr, {
    align = "center", y = -8, w = 300, h = 72,
    min = FMIN, max = FMAX, value = freq,
})

local function set_freq(f)
    if f < FMIN then f = FMIN end
    if f > FMAX then f = FMAX end
    freq = f
    readout:set_text(freq .. " Hz")
    if slider:get_value() ~= freq then slider:set_value(freq) end
end

slider:on("value_changed", function()
    set_freq(slider:get_value())
end)

local function play()
    if not have_audio then return end
    audio.tone(freq, 400)
end

local play_btn = lvgl.button(scr, {
    text = lvgl.symbol.play .. " Play",
    align = "bottom_mid", y = -34, w = 240, h = 120,
    bg_color = have_audio and "#2F80ED" or "#3A3A44",
    text_color = "#FFFFFF", radius = 16,
})
play_btn:on("clicked", play)

-- A dedicated line for the unavailable state, so a slider drag (which rewrites
-- the readout) can't erase it. Empty when the speaker is present.
if not have_audio then
    lvgl.label(scr, {
        text = "no speaker", align = "bottom_mid", y = -168,
        text_color = "#A0A0AE", font = lvgl.font(26),
    })
end

-- PWR mirrors Play -- an obvious binary action, also reachable on screen.
-- Bind on release (like stopwatch) so starting a >=6s power-off hold is silent.
button.on("pwr", "released", play)

scr:load()
