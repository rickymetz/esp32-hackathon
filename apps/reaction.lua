-- Reaction -- tap Start, wait for green, then tap as fast as you can. Shows
-- your time and tracks the best. Tapping before green is "too soon".
--
-- Timing note: apps have no high-resolution wall clock (os.clock is CPU
-- time), so elapsed time is accumulated by a 10ms timer -- fine for a game.
--
-- Install: ./.venv/bin/python tools/push.py apps/reaction.lua

local lvgl = require("lvgl")
local ui = require("ui")
local timer = require("timer")
local button = require("button")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

ui.title(scr, "Reaction")

local best_lbl = lvgl.label(scr, {
    text = "best: --", align = "top_mid", y = 64, text_color = "#A0A0AE",
})

local state = "idle"        -- idle | waiting | go | result
local best
local elapsed = 0
local wait_h, count_h

local pad = lvgl.button(scr, {
    text = "Start", align = "center", y = 24, w = 320, h = 250,
    bg_color = "#2f80ed", text_color = "#ffffff",
})
pad:set_style({ font = lvgl.font(40) })

local function set(txt, color)
    pad:set_text(txt)
    pad:set_style({ bg_color = color })
end

local function stop_timers()
    if wait_h then wait_h:cancel(); wait_h = nil end
    if count_h then count_h:cancel(); count_h = nil end
end

local function start_round()
    state = "waiting"
    set("Wait...", "#3a3a44")
    wait_h = timer.after(math.random(1200, 4000), function()
        wait_h = nil
        state = "go"
        set("TAP!", "#27ae60")
        elapsed = 0
        count_h = timer.every(10, function() elapsed = elapsed + 10 end)
    end)
end

-- One activation path, shared by the pad and PWR.
local function activate()
    if state == "idle" or state == "result" then
        start_round()
    elseif state == "waiting" then
        stop_timers()
        state = "result"
        set("Too soon!\nTap to retry", "#c0392b")
    elseif state == "go" then
        stop_timers()
        state = "result"
        if not best or elapsed < best then
            best = elapsed
            best_lbl:set_text("best: " .. best .. " ms")
        end
        set(elapsed .. " ms\nTap to retry", "#2f80ed")
    end
end

pad:on("clicked", activate)
button.on("pwr", "pressed", activate)

scr:load()
