-- Stopwatch -- start/stop/reset, updating every 100ms. Written from
-- docs/APP_CONTRACT.md alone, as that document's acceptance test.
--
-- Install: ./.venv/bin/python tools/push.py apps/stopwatch.lua

local lvgl = require("lvgl")
local timer = require("timer")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#101014" })

lvgl.label(scr, {
    text = "Stopwatch",
    align = "top_mid", y = 24,
    text_color = "#6a6a78",
})

-- A big font makes the readout legible; no font ships with the launcher by
-- default, so font_load raises an error if this file isn't on the card.
-- pcall guards that: fall back to the built-in font instead of crashing.
local display_font
local font_ok, font_result = pcall(lvgl.font_load, "apps/stopwatch_big.ttf", { size = 64 })
if font_ok then
    display_font = font_result
end

local display = lvgl.label(scr, {
    text = "00:00.0",
    align = "center", y = -90,
    text_color = "#ffffff",
})
if display_font then
    display:set_style({ font = display_font })
end

local elapsed_ms = 0
local running = false
local tick_handle

local function format_time(ms)
    local ds = math.floor(ms / 100)        -- deciseconds (tenths of a second)
    local minutes = math.floor(ds / 600)
    local seconds = math.floor(ds / 10) % 60
    local tenths = ds % 10
    return string.format("%02d:%02d.%d", minutes, seconds, tenths)
end

-- Big touch targets per the contract's ~200x100 rule; stacked so both fit
-- on a 368-wide screen.
local start_btn = lvgl.button(scr, {
    text = "Start",
    align = "bottom_mid", y = -160,
    w = 280, h = 110,
    bg_color = "#2f80ed",
    text_color = "#ffffff",
})

local reset_btn = lvgl.button(scr, {
    text = "Reset",
    align = "bottom_mid", y = -30,
    w = 280, h = 110,
    bg_color = "#3a3a44",
    text_color = "#ffffff",
})

local function stop_ticking()
    if tick_handle then
        tick_handle:cancel()
        tick_handle = nil
    end
end

start_btn:on("clicked", function()
    if running then
        running = false
        start_btn:set_text("Start")
        stop_ticking()
    else
        running = true
        start_btn:set_text("Stop")
        tick_handle = timer.every(100, function()
            elapsed_ms = elapsed_ms + 100
            display:set_text(format_time(elapsed_ms))
        end)
    end
end)

reset_btn:on("clicked", function()
    running = false
    start_btn:set_text("Start")
    stop_ticking()
    elapsed_ms = 0
    display:set_text(format_time(elapsed_ms))
end)

scr:load()

-- Return and let the launcher pump events. No while-true loop: the timer
-- above does the periodic work instead.
