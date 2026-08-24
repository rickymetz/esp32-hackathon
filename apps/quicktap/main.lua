-- Quicktap -- a self-contained folder app: code, icon, and high score all live
-- in apps/quicktap/ on the SD card. Nothing here is baked into the firmware.
--
--   apps/quicktap/main.lua   this file (the launcher runs it)
--   apps/quicktap/icon.png   the source icon (a dev edits this)
--   apps/quicktap/icon.bin   the launcher-ready icon (push.py builds it)
--   state/quicktap.json      the saved best score (require("store") writes it)
--
-- Install:  ./.venv/bin/python tools/push.py apps/quicktap
--
-- The game: tap the target as many times as you can before the clock runs out.
-- Your best score is remembered across runs and reboots.

local lvgl   = require("lvgl")
local ui     = require("ui")
local timer  = require("timer")
local store  = require("store")
local audio  = require("audio")
local button = require("button")

lvgl.init({ buffer_lines = 40 })

local ROUND_MS = 10000   -- ten seconds a round

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

ui.title(scr, "Quicktap")

-- store.get(key, default): the saved value, or the default the first time.
local best = store.get("best", 0)
local best_stat = ui.stat(scr, { value = best, label = "best", size = 40,
                                 align = "top_mid", y = 86 })

local hint = ui.note(scr, "Tap as fast as you can", { y = 150, size = 26 })

local count    = 0
local state    = "idle"     -- idle -> running -> done
local deadline = 0
local tick                  -- declared before use so the closure sees the local
local target                -- the big tap button, built below

local function finish_round()
    state = "done"
    if tick then tick:cancel(); tick = nil end
    audio.play({ { 660, 120 }, { 880, 200 } })
    if count > best then
        best = count
        -- get/set are in memory; save() is what actually writes the card.
        store.set("best", best)
        store.save()
        best_stat.set(best)
        hint:set_text("New best: " .. count .. " taps!")
    else
        hint:set_text("Time! " .. count .. " taps")
    end
    target:set_text("Play again")
end

local function start_round()
    count = 0
    state = "running"
    -- Absolute deadline, so a late tick can't stretch the round (the timer
    -- accuracy trap the app contract warns about).
    deadline = timer.now_ms() + ROUND_MS
    target:set_text("0")
    if tick then tick:cancel() end
    tick = timer.every(100, function()
        local left = deadline - timer.now_ms()
        if left <= 0 then
            finish_round()
            return
        end
        hint:set_text(string.format("%.1fs left", left / 1000))
    end)
end

target = ui.button(scr, {
    text = "Start",
    align = "center", y = 40,
    w = 300, h = 150,
    on_click = function()
        if state == "running" then
            count = count + 1
            target:set_text(tostring(count))
            audio.beep()
        else
            start_round()
        end
    end,
})

-- PWR is an accelerator, never the only path: the on-screen target starts a
-- round too (app contract, rule 2). It only (re)starts -- never a destructive
-- or hidden action.
button.on("pwr", "pressed", function()
    if state ~= "running" then start_round() end
end)

scr:load()
