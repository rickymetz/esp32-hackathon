-- Simon -- repeat the growing sequence. Watch the pads flash, then tap them
-- back in order; each round adds one. Tap any pad to start or to retry after a
-- miss.
--
-- Install: ./.venv/bin/python tools/push.py apps/simon.lua

local lvgl = require("lvgl")
local timer = require("timer")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

local status = lvgl.label(scr, {
    text = "Tap to start", align = "top_mid", y = 40, text_color = "#ffffff",
})
status:set_style({ font = lvgl.font(32) })

-- Forward declarations so the pad handlers can see the game functions.
local flash, on_pad, play, next_round, start_game

-- Two columns on a 368px panel cap pad width at ~180 (below the ~200 ideal),
-- so the pads are made as wide as the grid allows and extra tall (176) to keep
-- the hit area generous -- height is what the digitizer is most sensitive to.
local pads = {}
local function make_pad(idx, x, y, dim, bright)
    local p = lvgl.button(scr, { x = x, y = y, w = 180, h = 176, radius = 18, bg_color = dim })
    pads[idx] = { obj = p, dim = dim, bright = bright }
    p:on("clicked", function() on_pad(idx) end)
end

make_pad(1,   4, 88, "#5a1e1e", "#ff5b4a")   -- red
make_pad(2, 188, 88, "#14432a", "#52e08a")   -- green
make_pad(3,   4, 268, "#16324f", "#6db3ff")  -- blue
make_pad(4, 188, 268, "#4a3a12", "#ffd24a")  -- yellow

local seq = {}
local state = "idle"          -- idle | playing | input | wait | over
local input_pos = 1

flash = function(idx)
    local p = pads[idx]
    p.obj:set_style({ bg_color = p.bright })
    timer.after(300, function() p.obj:set_style({ bg_color = p.dim }) end)
end

play = function()
    state = "playing"
    status:set_text("Watch")
    local i = 0
    local h
    h = timer.every(650, function()
        i = i + 1
        if i > #seq then
            h:cancel()
            state = "input"
            input_pos = 1
            status:set_text("Your turn")
            return
        end
        flash(seq[i])
    end)
end

next_round = function()
    seq[#seq + 1] = math.random(1, 4)
    play()
end

start_game = function()
    seq = {}
    next_round()
end

on_pad = function(idx)
    if state == "idle" or state == "over" then
        start_game()
        return
    end
    if state ~= "input" then return end     -- ignore taps while watching

    flash(idx)
    if idx == seq[input_pos] then
        input_pos = input_pos + 1
        if input_pos > #seq then
            status:set_text("Score: " .. #seq)
            state = "wait"
            timer.after(700, next_round)     -- next round after a beat
        end
    else
        state = "over"
        status:set_text("Wrong! " .. (#seq - 1) .. " -- tap to retry")
    end
end

scr:load()
