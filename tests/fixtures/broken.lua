local lvgl = require("lvgl")
lvgl.init({ buffer_lines = 40 })
local scr = lvgl.create_screen()
scr:load()
local t = nil
print(t.field)   -- attempt to index a nil value
