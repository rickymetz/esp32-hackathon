-- Settings -- device preferences. For now: font size (the global UI scale).
-- Stepping the value re-scales the whole UI; the preview shows the effect and
-- the choice is saved so it sticks across restarts.
--
-- Install: ./.venv/bin/python tools/push.py apps/settings.lua

local lvgl = require("lvgl")
local ui = require("ui")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

ui.title(scr, "Font size")

-- Live preview: re-styling with lvgl.font() picks up the new scale.
local preview = lvgl.label(scr, {
    text = "Sample Aa 123", align = "center", y = -30, text_color = "#FFFFFF",
})
local function refresh_preview()
    preview:set_style({ font = lvgl.font(40) })
end

-- Persist the choice so the launcher can restore it at boot. The sim has no
-- /sdcard, so the write is best-effort (pcall) -- the in-session scale still
-- applies either way.
local CONFIG = "/sdcard/font_scale.txt"
local function persist(scale)
    local ok, f = pcall(io.open, CONFIG, "w")
    if ok and f then
        f:write(tostring(scale))
        f:close()
    end
end

local pct = math.floor(lvgl.font_scale() * 100 + 0.5)
ui.note(scr, "smaller  <  UI text  >  larger", { y = 40, size = 26 })

local stepper
stepper = ui.stepper(scr, { min = 70, max = 130, step = 10, value = pct, label = "%d%%" },
    function(v)
        lvgl.font_scale(v / 100)
        persist(v / 100)
        refresh_preview()
    end)
stepper.row:align("bottom_mid", 0, -28)

refresh_preview()
scr:load()
