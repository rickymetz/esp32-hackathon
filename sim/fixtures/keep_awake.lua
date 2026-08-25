-- Exercises lvgl.keep_awake's Lua contract against the shared binding.
-- Not an app; lives outside apps/ so the launcher never lists it.
local lvgl = require("lvgl")

lvgl.init({ buffer_lines = 40 })
local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })
scr:load()

print("DEFAULT " .. tostring(lvgl.keep_awake()))
print("SET_TRUE " .. tostring(lvgl.keep_awake(true)))
print("READBACK " .. tostring(lvgl.keep_awake()))
print("SET_FALSE " .. tostring(lvgl.keep_awake(false)))
print("READBACK2 " .. tostring(lvgl.keep_awake()))
print("KADONE")
