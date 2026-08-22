-- Exercises every data-widget method documented in APP_CONTRACT.md's
-- "The data widgets, and how to fill them". If a documented signature is
-- wrong, this errors and the run fails.
local lvgl = require("lvgl")
lvgl.init({ buffer_lines = 40 })
local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

local function try(name, fn)
    local ok, err = pcall(fn)
    print(string.format("WAPI %-34s %s", name, ok and "ok" or ("FAIL " .. tostring(err))))
end

local c = lvgl.chart(scr, { type = "line", point_count = 10, min = 0, max = 100, w = 200, h = 100 })
local series
try("chart:add_series",        function() series = c:add_series("#2F80ED") end)
try("chart:set_series_values", function() c:set_series_values(series, { 10, 40, 25, 90 }) end)
try("chart:set_next_value",    function() c:set_next_value(series, 55) end)
try("chart:set_type",          function() c:set_type("line") end)
try("chart:set_point_count",   function() c:set_point_count(20) end)
try("chart:set_range",         function() c:set_range(0, 100, "primary_y") end)
try("chart:refresh",           function() c:refresh() end)

local t = lvgl.table(scr, { w = 200, h = 100 })
try("table:set_cell",          function() t:set_cell(1, 1, "Mon") end)
try("table:get_cell",          function() assert(t:get_cell(1, 1) == "Mon") end)

local l = lvgl.list(scr, { w = 200, h = 100 })
try("list:add_text",           function() l:add_text("Section") end)
try("list:add_button",         function() local r = l:add_button(lvgl.symbol.file, "Open"); assert(r) end)

local tv = lvgl.tabview(scr, { w = 200, h = 100 })
local page
try("tabview:add_tab",         function() page = tv:add_tab("Stats"); assert(page) end)
try("tabview:set_active",      function() tv:set_active(1) end)
try("tabview:get_active",      function() assert(tv:get_active()) end)
try("tabview:get_tab_count",   function() assert(tv:get_tab_count() >= 1) end)
try("tabview:set_tab_text",    function() tv:set_tab_text(1, "New") end)

local m = lvgl.msgbox(scr, {})
try("msgbox:add_title",        function() m:add_title("Delete?") end)
try("msgbox:add_text",         function() m:add_text("Cannot be undone.") end)
try("msgbox:add_footer_button",function() m:add_footer_button("Cancel") end)
try("msgbox:add_close_button", function() m:add_close_button() end)

local sb = lvgl.spinbox(scr, { min = 0, max = 100, value = 5 })
try("spinbox:set_step",        function() sb:set_step(10) end)
try("spinbox:get_step",        function() assert(sb:get_step()) end)
try("spinbox:increment",       function() sb:increment() end)
try("spinbox:decrement",       function() sb:decrement() end)
try("spinbox:step_next",       function() sb:step_next() end)
try("spinbox:step_prev",       function() sb:step_prev() end)

local led = lvgl.led(scr, { color = "#00ff00", brightness = 180, on = true })
try("led:set_color",           function() led:set_color("#00FF00") end)
try("led:set_brightness",      function() led:set_brightness(200) end)
try("led:get_brightness",      function() assert(led:get_brightness()) end)
try("led:on/off/toggle",       function() led:on(); led:off(); led:toggle() end)

local bm = lvgl.buttonmatrix(scr, { w = 200, h = 100 })
try("buttonmatrix:set_map",    function() bm:set_map({ "1", "2", "3", "\n", "4", "5", "6" }) end)
try("buttonmatrix:set_selected", function() bm:set_selected(1) end)
try("buttonmatrix:get_selected", function() bm:get_selected() end)
try("buttonmatrix:get_button_text", function() bm:get_button_text(1) end)
try("buttonmatrix:set_one_checked", function() bm:set_one_checked(true) end)

local cal = lvgl.calendar(scr, { w = 200, h = 200 })
try("calendar:set_today",      function() cal:set_today(2026, 8, 22) end)
try("calendar:set_shown",      function() cal:set_shown(2026, 8) end)
try("calendar:set_highlighted",function() cal:set_highlighted({ { 2026, 8, 22 }, { 2026, 8, 25 } }) end)
try("calendar:get_pressed_date", function() cal:get_pressed_date() end)

local cv = lvgl.canvas(scr, { w = 64, h = 64 })
try("canvas:fill_bg",          function() cv:fill_bg("#000000", 255) end)
try("canvas:set_px",           function() cv:set_px(1, 1, "#FF0000", 255) end)
try("canvas:get_px",           function() cv:get_px(1, 1) end)

print("WAPI done")
