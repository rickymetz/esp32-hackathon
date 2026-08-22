-- Stopwatch: the worked example for timers, the hero font, and the PWR
-- button. Referenced from docs/APP_CONTRACT.md -- keep it exemplary:
--   * elapsed time comes from timer.now_ms() wall-clock timestamps, NOT
--     from counting ticks -- periodic timers re-arm from dispatch time,
--     so tick-counting drifts slow without bound
--   * hero number via lvgl.font(60) -- compiled in, cannot go missing
--   * PWR = lap, with an on-screen Lap button too: the button is an
--     accelerator, never the only path (contract button rule 2). Bound
--     to "released", not "pressed", so starting a >=6s power-off hold
--     cannot clobber a recorded lap on the way down.
--   * builds the UI and returns; a timer only *renders*

local lvgl = require("lvgl")
local timer = require("timer")
local button = require("button")
local ui = require("ui")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

ui.title(scr, "Stopwatch")

local accum_ms = 0        -- time banked across previous runs
local started_at = nil    -- timer.now_ms() at Start; nil = stopped

local function elapsed()
    if started_at then
        return accum_ms + (timer.now_ms() - started_at)
    end
    return accum_ms
end

local readout = lvgl.label(scr, {
    text = "00:00.0",
    align = "top_mid", y = 96,
    text_color = "#FFFFFF",
    font = lvgl.font(60),
})

local lap_label = lvgl.label(scr, {
    text = "Lap or PWR = split",
    align = "top_mid", y = 176,
    text_color = "#A0A0AE",
})

local function fmt(ms)
    return string.format("%02d:%04.1f", ms // 60000, (ms % 60000) / 1000)
end

local start_btn = lvgl.button(scr, {
    text = "Start",
    align = "bottom_mid", y = -136,
    w = 344, h = 104,
    bg_color = "#2F80ED", text_color = "#FFFFFF", radius = 12,
})

local lap_btn = lvgl.button(scr, {
    text = "Lap",
    align = "bottom_left", x = 12, y = -16,
    w = 164, h = 104,
    bg_color = "#1E1E28", text_color = "#FFFFFF", radius = 12,
})

local reset_btn = lvgl.button(scr, {
    text = "Reset",
    align = "bottom_right", x = -12, y = -16,
    w = 164, h = 104,
    bg_color = "#1E1E28", text_color = "#FFFFFF", radius = 12,
})

start_btn:on("clicked", function()
    if started_at then
        accum_ms = elapsed()
        started_at = nil
        start_btn:set_text("Start")
    else
        started_at = timer.now_ms()
        start_btn:set_text("Stop")
    end
end)

local function lap()
    lap_label:set_text("lap  " .. fmt(elapsed()))
end

lap_btn:on("clicked", lap)

reset_btn:on("clicked", function()
    started_at = nil
    accum_ms = 0
    start_btn:set_text("Start")
    readout:set_text(fmt(0))
    lap_label:set_text("Lap or PWR = split")
end)

-- PWR is the eyes-free accelerator for the same on-screen Lap action.
button.on("pwr", "released", lap)

-- This timer only renders; it never keeps time.
timer.every(100, function()
    if started_at then
        readout:set_text(fmt(elapsed()))
    end
end)

scr:load()
