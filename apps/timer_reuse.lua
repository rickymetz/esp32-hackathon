-- Verifies that timer slot reuse cannot be killed by a stale handle.
--
-- Sequence:
--   1. A one-shot fires and its slot is freed once its callback returns
--      (app_timer_run_due only unrefs *after* the callback completes, so
--      the reuse step below must happen on a LATER pump, not inside the
--      one-shot's own callback).
--   2. A second, later one-shot creates a new repeating timer. Because free
--      slots are picked by a linear scan from slot 0, this lands in the
--      same slot the first one-shot just vacated.
--   3. The (now stale) handle from step 1 is cancelled.
--   4. If the stale cancel silently killed the repeating timer, no more
--      ticks would print. If the generation counter caught it, ticks
--      keep coming -- proving the fix.
local lvgl  = require("lvgl")
local timer = require("timer")

lvgl.init({ buffer_lines = 40 })
local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

local label = lvgl.label(scr, {
    text = "timer reuse test", align = "center", text_color = "#ffffff",
})

local live_ticks = 0
local live_handle

print("STEP1 creating one-shot (will free its slot when it fires)")
local oneshot_handle
oneshot_handle = timer.after(100, function()
    print("ONESHOT fired (slot now free)")
end)

timer.after(300, function()
    print("STEP2 creating repeating timer (should reuse the freed slot)")
    live_handle = timer.every(150, function()
        live_ticks = live_ticks + 1
        label:set_text("live ticks: " .. live_ticks)
        print("LIVE TICK " .. live_ticks)
        if live_ticks == 5 then
            live_handle:cancel()
            print("LIVE TIMER CANCELLED (test complete)")
        end
    end)

    print("STEP3 cancelling stale one-shot handle")
    oneshot_handle:cancel()
    print("STALE CANCEL DONE")
end)

scr:load()
