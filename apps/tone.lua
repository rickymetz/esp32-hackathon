-- Tone -- a signal generator for the speaker.
--
-- Drag the slider to pick a frequency, tap Play (or press PWR) to sound it for
-- 400ms through audio.tone(). A pad-of-tiny-piano-keys would fight this
-- digitizer (the >=200x100 rule), so the control is one wide slider and one big
-- button instead -- which also matches what the audio module actually is:
-- synthesised tones, not samples. Two preset chips jump to concert A (440) and
-- middle C (262).
--
-- audio is an accelerator, not the only path: everything here is on-screen, and
-- PWR just mirrors Play. In the simulator the speaker is silent (audio no-ops)
-- but the whole UI and the tone() calls run, so the app is verified board-free;
-- the sound itself is confirmed on hardware.

local lvgl = require("lvgl")
local audio = require("audio")
local button = require("button")

lvgl.init({ buffer_lines = 40 })

local FMIN, FMAX = 100, 2000
local freq = 440
local have_audio = audio.available()

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

lvgl.label(scr, { text = "Tone", align = "top_mid", y = 24,
                  text_color = "#FFFFFF", font = lvgl.font(32) })

local readout = lvgl.label(scr, {
    text = freq .. " Hz", align = "top_mid", y = 92,
    text_color = "#2F80ED", font = lvgl.font(60),
})

local slider = lvgl.slider(scr, {
    align = "center", y = -40, w = 300, h = 28,
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

-- Preset chips: quick jumps to two common references.
local function preset(label, f, x)
    local b = lvgl.button(scr, {
        text = label, align = "center", x = x, y = 20, w = 132, h = 88,
        bg_color = "#1E1E28", text_color = "#FFFFFF", radius = 14,
    })
    b:on("clicked", function()
        set_freq(f)
        play()
    end)
end
preset("A 440", 440, -78)
preset("C 262", 262, 78)

local play_btn = lvgl.button(scr, {
    text = lvgl.symbol.play .. " Play",
    align = "bottom_mid", y = -30, w = 240, h = 120,
    bg_color = have_audio and "#2F80ED" or "#3A3A44",
    text_color = "#FFFFFF", radius = 16,
})
play_btn:on("clicked", play)

if not have_audio then
    readout:set_text("no speaker")
    readout:set_style({ text_color = "#A0A0AE" })
end

-- PWR mirrors Play -- an obvious binary action, also reachable on screen.
button.on("pwr", "pressed", play)

scr:load()
