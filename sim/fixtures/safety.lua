-- Regression fixture for the memory-safety fixes found by adversarial review.
--
-- Every case here was reachable from ordinary app Lua and either panicked the
-- board or wrote through freed/read-only memory. They must now RAISE (a clean
-- Lua error an app can pcall) rather than corrupt anything.
local lvgl = require("lvgl")
lvgl.init({ buffer_lines = 40 })
local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

local function must_raise(name, fn)
    local ok, err = pcall(fn)
    print(string.format("SAFE %-40s %s", name,
        (not ok) and "ok" or "FAIL did not raise"))
end

local function must_survive(name, fn)
    local ok, err = pcall(fn)
    print(string.format("SAFE %-40s %s", name,
        ok and "ok" or ("FAIL " .. tostring(err))))
end

-- 1. A built-in font wraps a const lv_font_t in .rodata. Resizing it used to
--    write straight through that pointer -> StoreProhibited panic.
must_raise("builtin font set_size raises", function()
    lvgl.font(32):set_size(40)
end)

-- 2. A series belongs to ONE chart, and its point array is sized by THAT
--    chart. Driven against a chart with a larger point_count, LVGL walks the
--    bigger count over the smaller array: an out-of-bounds heap write whose
--    length and contents the caller picks. Verified: without the bind check
--    this segfaults the simulator (SIGSEGV, not a Lua error).
--
--    Direction matters. Short-series-into-long-chart is the corrupting one;
--    long-series-into-short-chart writes inside the larger array and looks
--    harmless, which is how it could sit here unnoticed.
local short = lvgl.chart(scr, { type = "line", point_count = 2,    min = 0, max = 100, w = 100, h = 60 })
local long  = lvgl.chart(scr, { type = "line", point_count = 4000, min = 0, max = 100, w = 100, h = 60 })
local s_short = short:add_series("#2F80ED")
local s_long  = long:add_series("#27AE60")

must_raise("short series into long chart raises", function()
    local big = {}
    for i = 1, 4000 do big[i] = 0x41414141 end
    long:set_series_values(s_short, big)
end)
must_raise("cross-chart set_next_value raises", function()
    short:set_next_value(s_long, 42)
end)
must_raise("cross-chart set_series_values raises", function()
    short:set_series_values(s_long, { 1, 2, 3 })
end)
must_survive("same-chart series still works", function()
    local s = short:add_series("#EB5757")
    short:set_next_value(s, 42)
    short:set_series_values(s, { 1, 2 })
end)

-- 3. A widget created without keeping its handle, then collected before the
--    screen is torn down, left the C record pointing at freed Lua userdata.
--    Deleting the widget afterwards WROTE through that pointer.
--
--    Unlike 1 and 2, this one is confirmed by READING the delete path
--    (`record->ud->record = NULL`) and NOT by an observed failure: unfixed,
--    this case still exits 0 here, because desktop malloc leaves the freed
--    userdata mapped and the write lands somewhere harmless. It is kept as a
--    regression guard, not as a reproduction.
must_survive("collected widget handle then delete", function()
    local host = lvgl.label(scr, { text = "host" })
    for i = 1, 40 do
        lvgl.label(scr, { text = "drop " .. i })   -- handle deliberately dropped
    end
    collectgarbage("collect")
    collectgarbage("collect")
    host:delete()
end)

-- 4. The arbitrary-pointer indev entry point must simply not be there.
must_survive("indev_register is not exposed", function()
    assert(lvgl.indev_register == nil, "indev_register is still on the module table")
end)

scr:load()
print("SAFE done")
