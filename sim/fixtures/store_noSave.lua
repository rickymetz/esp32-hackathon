-- Sets a value and NEVER calls store.save(). The launcher must write it on
-- exit; before that it was silently lost, and BOOT can land at any moment.
local lvgl = require("lvgl")
local store = require("store")
lvgl.init({ buffer_lines = 40 })
local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

local runs = store.get("runs", 0)
print("STORE read runs=" .. tostring(runs))
store.set("runs", runs + 1)          -- deliberately no save()
lvgl.label(scr, { text = "runs " .. runs, align = "center", text_color = "#ffffff" })
scr:load()
print("STORE done")
