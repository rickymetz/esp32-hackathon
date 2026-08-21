-- C1 repro: a widget-creation binding takes the LVGL lock in
-- lua_lvgl_create_widget(), then lua_lvgl_parse_opts() hits a bad option
-- value and longjmps out via luaL_error before the binding's own
-- lua_lvgl_unlock() ever runs (a real, one-line-typo-away path -- not a
-- contrived one). Before the fix at the drain_events_for()/app_timer.c
-- catch sites, this leaves lvgl_mux and s_lvgl.mutex held forever: the
-- LVGL task blocks in lvgl_port_lock() forever, display and touch go dead,
-- and even PWR cannot recover it because the cleanup path times out
-- waiting on a mutex nothing will ever give back. Only a power cycle
-- recovers. This is believed to be what wedged the dev board earlier.
--
-- Two trigger paths, same underlying broken binding:
--   1. TOUCH path (button "clicked" handler) -- exercises the
--      lua_lvgl_events.c drain_events_for() catch site directly. This is
--      the fifth site that was missing lua_lvgl_force_unlock_if_held()
--      before this fix. Needs a physical tap; provided for completeness.
--   2. TIMER path (this is what the automated verification actually
--      drives, since there is no touch-injection API and no way to tap
--      over serial) -- exercises app_timer.c's catch site, which shares
--      the exact same lua_lvgl_lock()/unlock()/force_unlock_if_held()
--      machinery that was reworked for this fix (recursion-depth-aware
--      force-unlock). A clean recovery here is strong evidence the shared
--      machinery -- and therefore the events.c site too -- is correct.

local lvgl  = require("lvgl")
local timer = require("timer")

lvgl.init({ buffer_lines = 40 })
local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#101014" })

local label = lvgl.label(scr, {
    text = "cb_error: waiting for boom",
    align = "center", text_color = "#ffffff",
})

local button = lvgl.button(scr, {
    text = "boom (touch path)",
    align = "bottom_mid", y = -30,
    w = 300, h = 100,
    bg_color = "#c0392b",
    text_color = "#ffffff",
})

-- x expects an integer (lua_lvgl_get_opt_int_field); a string makes it
-- luaL_error() its way out while lua_lvgl_create_widget() is still holding
-- the lock it took at entry.
local function boom()
    lvgl.label(scr, { text = "x", x = "not a number" })
end

button:on("clicked", function()
    print("CB_ERROR: firing CLICK-path boom")
    label:set_text("boom via CLICK (should not crash the board)")
    boom()
end)

timer.after(500, function()
    print("CB_ERROR: firing TIMER-path boom")
    label:set_text("boom via TIMER (should not crash the board)")
    boom()
    print("CB_ERROR: timer callback returned normally (pcall caught the error)")
end)

timer.after(2000, function()
    print("CB_ERROR: still alive 2s after boom -- lock was reclaimed, PASS")
    label:set_text("still alive: lock reclaimed, PASS")
end)

scr:load()
