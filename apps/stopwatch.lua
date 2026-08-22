-- Stopwatch: the worked example for timers, the hero font, and the PWR
-- button. Referenced from docs/APP_CONTRACT.md -- keep it exemplary:
--   * hero number via lvgl.font(60) -- compiled in, cannot go missing
--   * PWR = lap: binary, eyes-free, reversible -- exactly what the
--     button rules allow (see the contract's Physical buttons section)
--   * true black background, ui.title, whole-width buttons
--   * builds the UI and returns; timer.every does the ticking

local lvgl = require("lvgl")
local timer = require("timer")
local button = require("button")
local ui = require("ui")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

ui.title(scr, "Stopwatch")

local elapsed_ms = 0
local running = false

local readout = lvgl.label(scr, {
    text = "00:00.0",
    align = "top_mid", y = 96,
    text_color = "#FFFFFF",
    font = lvgl.font(60),
})

local lap_label = lvgl.label(scr, {
    text = "PWR = lap",
    align = "top_mid", y = 176,
    text_color = "#8A8A99",
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

local reset_btn = lvgl.button(scr, {
    text = "Reset",
    align = "bottom_mid", y = -16,
    w = 344, h = 104,
    bg_color = "#1E1E28", text_color = "#FFFFFF", radius = 12,
})

start_btn:on("clicked", function()
    running = not running
    start_btn:set_text(running and "Stop" or "Start")
end)

reset_btn:on("clicked", function()
    running = false
    elapsed_ms = 0
    start_btn:set_text("Start")
    readout:set_text(fmt(0))
    lap_label:set_text("PWR = lap")
end)

-- PWR records a lap: one press, immediate, reversible, works without
-- looking. An on-screen path to the same action is the contract's rule 2;
-- here Reset covers recovery and the lap is display-only.
button.on("pwr", "pressed", function()
    if running then
        lap_label:set_text("lap  " .. fmt(elapsed_ms))
    end
end)

timer.every(100, function()
    if running then
        elapsed_ms = elapsed_ms + 100
        readout:set_text(fmt(elapsed_ms))
    end
end)

scr:load()
