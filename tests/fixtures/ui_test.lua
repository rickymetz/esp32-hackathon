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
tv:set_scroll({ scrollbar = "off" })

local p1 = tv:add_tile(1, 1, "right")
local p2 = tv:add_tile(2, 1, "hor")
local p3 = tv:add_tile(3, 1, "left")

ui.dots(scr, tv, { count = 3 })

-- ---- page 1 ----

-- Root pages get no back control (nothing to close; BOOT is the exit).
ui.title(p1, "UI test")

local list1 = lvgl.container(p1, {
    x = 0, y = 100, w = 368, h = 300,
    bg_opa = 0, border_width = 0, pad = 12,
})
list1:set_flex({ flow = "column", pad_row = 16 })

ui.row(list1, {
    text = "Toggle me", kind = "toggle", checked = true,
    on_change = function() ui.toast(scr, "toggled") end,
})
list1:set_scroll({ scrollbar = "active" })

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

local name = ""
local name_row
name_row = ui.row(list1, {
    text = "Name: not set", kind = "nav",
    on_click = function()
        -- Pass the current value and keep it on cancel -- the old demo
        -- overwrote the name with "(cancelled)" and always restarted
        -- from empty, teaching both anti-patterns (review).
        keyboard.open({ title = "Name", mode = "text", initial = name }, function(t)
            if t then
                name = t
                name_row.label:set_text("Name: " .. (name == "" and "not set" or name))
            end
        end)
    end,
})

-- ---- page 2 ----

ui.title(p2, "Select")

local list2 = lvgl.container(p2, {
    x = 0, y = 70, w = 368, h = 236,
    bg_opa = 0, border_width = 0, pad = 12,
})
list2:set_flex({ flow = "column", pad_row = 16 })

ui.select(list2, { options = { "Small", "Medium" }, selected = 2 },
    function(i) ui.toast(scr, "picked " .. i) end)

local danger = lvgl.button(p2, {
    text = lvgl.symbol.trash .. " Delete all",
    align = "bottom_mid", y = -28,
    w = 344, h = 104,
    bg_color = "#B3261E", text_color = "#FFFFFF", radius = 12,
})
danger:on("clicked", function()
    ui.confirm({ title = "Delete all?", message = "This is only a test.",
                 confirm_label = "Delete", destructive = true },
        function(yes) ui.toast(scr, yes and "confirmed" or "cancelled") end)
end)

-- ---- page 3 ----

ui.title(p3, "Fill")

local bright = 40
local bright_btn = lvgl.button(p3, {
    text = "Brightness: 40%",
    align = "center",
    w = 344, h = 104,
    bg_color = "#1E1E28", text_color = "#FFFFFF", radius = 12,
})
local reps = ui.stepper(p3, { min = 0, max = 20, value = 5, label = "%d reps" },
    function(v) end)
reps.row:align("bottom_mid", 0, -40)

bright_btn:on("clicked", function()
    ui.fill({ title = "Brightness", min = 0, max = 100, value = bright, label = "%d%%" },
        nil,
        function(v)
            bright = v
            bright_btn:set_text("Brightness: " .. v .. "%")
        end)
end)

scr:load()
