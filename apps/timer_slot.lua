-- I5 repro: a repeating timer's callback cancels itself and immediately
-- creates a new one-shot from inside that same callback. timer_add() picks
-- the first free slot by linear scan, so the new one-shot lands in the
-- exact slot the repeating timer just vacated -- the same slot index
-- app_timer_run_due() is still iterating over in its for loop.
--
-- Before the fix, app_timer_run_due() re-read that slot after the pcall
-- and only checked `ref ~= LUA_NOREF` -- true for the *new* occupant too --
-- so it applied the *old* timer's post-processing to the *new* timer's
-- slot. Since the new one-shot's period_us is 0, that took the "one-shot
-- fired" branch: immediately luaL_unref'd it and set ref = LUA_NOREF,
-- freeing it before it ever got a chance to fire on a later pump.
--
-- The fix snapshots `gen` before the pcall and skips post-processing if it
-- changed underneath -- `gen` is bumped by timer_add() on every (re)alloc
-- of a slot, so a slot takeover is unmistakable.

local lvgl  = require("lvgl")
local timer = require("timer")

lvgl.init({ buffer_lines = 40 })
local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

local label = lvgl.label(scr, {
    text = "timer_slot: waiting", align = "center", text_color = "#ffffff",
})

local fired = false

local h
h = timer.every(150, function()
    print("TIMER_SLOT: repeating timer fired, cancelling self + creating new one-shot")
    h:cancel()
    timer.after(50, function()
        fired = true
        print("TIMER_SLOT: NEW ONESHOT FIRED -- slot reuse survived")
        label:set_text("NEW ONESHOT FIRED")
    end)
end)

timer.after(2000, function()
    if fired then
        print("TIMER_SLOT: PASS")
        label:set_text("TIMER_SLOT: PASS")
    else
        print("TIMER_SLOT: FAIL -- new one-shot never fired")
        label:set_text("TIMER_SLOT: FAIL")
    end
end)

scr:load()
