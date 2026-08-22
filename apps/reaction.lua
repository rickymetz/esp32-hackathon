-- Reaction -- tap Start, wait for green, then tap as fast as you can. Shows
-- your time and tracks the best. Tapping before green is "too soon".
--
-- Timing note: the reaction time comes from timer.now_ms() timestamps, which
-- is monotonic milliseconds since boot. An earlier version accumulated it in a
-- 10 ms timer (elapsed = elapsed + 10), and that is wrong in the one way this
-- app cannot afford: a periodic timer re-arms AFTER its callback, so a "10 ms"
-- tick really costs 10 ms plus dispatch. Counting ticks therefore reports far
-- LESS time than actually passed -- a reaction game that flatters you.
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
local go_at                 -- timer.now_ms() when the pad turned green
local wait_h

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
end

local function start_round()
    state = "waiting"
    set("Wait...", "#3a3a44")
    wait_h = timer.after(math.random(1200, 4000), function()
        wait_h = nil
        state = "go"
        set("TAP!", "#27ae60")
        go_at = timer.now_ms()
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
        local elapsed = timer.now_ms() - go_at
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
