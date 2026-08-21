-- ui/keyboard module test: exercises all eight ui helpers and both
-- keyboard modes. Three tileview pages + ui.dots.
--
--   page 1  header (back = toast), rows: toggle / nav->picker / keyboard
--   page 2  ui.select group; ui.confirm behind a destructive button
--   page 3  ui.fill drag surface
--   BOOT    exits (as everywhere)

local lvgl = require("lvgl")
local ui = require("ui")
local keyboard = require("keyboard")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

local tv = lvgl.tileview(scr, {})
tv:set_style({ bg_color = "#000000", bg_opa = 0 })

local p1 = tv:add_tile(1, 1, "right")
local p2 = tv:add_tile(2, 1, "hor")
local p3 = tv:add_tile(3, 1, "left")

ui.dots(scr, tv, { count = 3 })

-- ---- page 1 ----

ui.header(p1, {
    title = "UI test",
    kind = "sheet",
    on_back = function() ui.toast(scr, "toast works") end,
})

local list1 = lvgl.container(p1, {
    x = 0, y = 100, w = 368, h = 300,
    bg_opa = 0, border_width = 0, pad = 12,
})
list1:set_flex({ flow = "column", pad_row = 16 })

ui.row(list1, {
    text = "Toggle me", kind = "toggle", checked = true,
    on_change = function() ui.toast(scr, "toggled") end,
})

local fruit = { "Apple", "Banana", "Cherry", "Durian" }
local fruit_row
fruit_row = ui.row(list1, {
    text = "Fruit: Apple", kind = "nav",
    on_click = function()
        ui.picker({ title = "Fruit", options = fruit, selected = 1, disabled = { 4 } },
            function(i)
                if i then fruit_row.label:set_text("Fruit: " .. fruit[i]) end
            end)
    end,
})

local name_row
name_row = ui.row(list1, {
    text = "Name: ?", kind = "nav",
    on_click = function()
        keyboard.open({ title = "Name", mode = "text" }, function(t)
            name_row.label:set_text("Name: " .. (t or "(cancelled)"))
        end)
    end,
})

-- ---- page 2 ----

lvgl.label(p2, { text = "Select + confirm", align = "top_mid", y = 24, text_color = "#FFFFFF" })

local list2 = lvgl.container(p2, {
    x = 0, y = 70, w = 368, h = 240,
    bg_opa = 0, border_width = 0, pad = 12,
})
list2:set_flex({ flow = "column", pad_row = 16 })

ui.select(list2, { options = { "Small", "Medium" }, selected = 2 },
    function(i) ui.toast(scr, "picked " .. i) end)

local danger = lvgl.button(p2, {
    text = lvgl.symbol.trash .. " Delete all",
    align = "bottom_mid", y = -60,
    w = 344, h = 104,
    bg_color = "#B3261E", text_color = "#FFFFFF", radius = 12,
})
danger:on("clicked", function()
    ui.confirm({ title = "Delete all?", message = "This is only a test.",
                 confirm_label = "Delete", destructive = true },
        function(yes) ui.toast(scr, yes and "confirmed" or "cancelled") end)
end)

-- ---- page 3 ----

ui.fill(p3, { min = 0, max = 100, value = 40, label = "%d%%" },
    function() end)

scr:load()
