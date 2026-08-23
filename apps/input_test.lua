-- Input foundations test: button module, gestures, fonts, symbols,
-- active_screen. Three tileview pages plus a non-scrolling gesture screen.
--
-- What to check by hand, per the plan's verification list:
--   page 1  self-checks all PASS; PWR press/release/long_pressed counts move
--   page 2  swiping pages fires value_changed (page label updates)
--   page 3  "Gesture screen" opens a non-scrolling screen where all four
--           swipe directions report; its Back button returns via
--           lvgl.active_screen() captured at open time
--   BOOT    returns to the launcher from anywhere

local lvgl = require("lvgl")
local button = require("button")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

local tv = lvgl.tileview(scr, {})
tv:set_style({ bg_color = "#000000", bg_opa = 0 })

local p1 = tv:add_tile(1, 1, "right")
local p2 = tv:add_tile(2, 1, "hor")
local p3 = tv:add_tile(3, 1, "left")

-- ---- page 1: self-checks + PWR counters ----

local big = lvgl.font(40)

lvgl.label(p1, {
    text = lvgl.symbol.play .. " Input test",
    align = "top_mid", y = 16,
    text_color = "#ffffff",
    font = big,
})

-- Each check is expected to RAISE; pcall-succeeding means the guard failed.
local checks = {
    { "font(41) rejected",  not pcall(lvgl.font, 41) },
    { "boot reserved",      not pcall(button.on, "boot", "pressed", function() end) },
    { "gesture needs scr",  not pcall(function()
                                local l = lvgl.label(p1, { text = "" })
                                l:on("gesture", function() end)
                            end) },
    { "font(32) works",     pcall(lvgl.font, 32) },
    { "active_screen works", lvgl.active_screen() ~= nil },
}

local y = 80
for _, c in ipairs(checks) do
    lvgl.label(p1, {
        text = (c[2] and lvgl.symbol.ok or lvgl.symbol.warning) .. " " .. c[1],
        align = "top_left", x = 12, y = y,
        text_color = c[2] and "#8aff8a" or "#ff6b6b",
    })
    y = y + 36
end

local presses, releases, longs = 0, 0, 0
local pwr_label = lvgl.label(p1, {
    text = "PWR: press it",
    align = "bottom_mid", y = -16,
    text_color = "#ffffff",
})

local function update_pwr()
    pwr_label:set_text(("P:%d R:%d L:%d down:%s")
        :format(presses, releases, longs, tostring(button.is_down("pwr"))))
end

button.on("pwr", "pressed", function() presses = presses + 1; update_pwr() end)
button.on("pwr", "released", function() releases = releases + 1; update_pwr() end)
button.on("pwr", "long_pressed", function() longs = longs + 1; update_pwr() end)

-- ---- page 2: paging fires value_changed, not gesture ----

local page_label = lvgl.label(p2, {
    text = "page ?",
    align = "center",
    text_color = "#ffffff",
    font = big,
})
lvgl.label(p2, {
    text = "swipe between pages;\nthis label tracks value_changed",
    align = "bottom_mid", y = -24,
    text_color = "#A0A0AE",
})

tv:on("value_changed", function()
    -- get_active_tile returns the tile object; col/row live on it
    page_label:set_text("page moved")
end)

-- ---- page 3: open a non-scrolling gesture screen ----

lvgl.label(p3, { text = "Gesture test", align = "top_mid", y = 24, text_color = "#ffffff" })

local open_btn = lvgl.button(p3, {
    text = "Gesture screen",
    align = "center",
    w = 240, h = 120,
    bg_color = "#2f80ed", text_color = "#ffffff",
})

open_btn:on("clicked", function()
    local caller = lvgl.active_screen()

    local gs = lvgl.create_screen()
    gs:set_style({ bg_color = "#000000" })

    local dir_label = lvgl.label(gs, {
        text = "swipe any direction",
        align = "center",
        text_color = "#ffffff",
        font = big,
    })

    gs:on("gesture", function()
        dir_label:set_text("swipe: " .. tostring(lvgl.gesture_dir()))
    end)

    local back = lvgl.button(gs, {
        text = lvgl.symbol.left .. " Back",
        align = "bottom_mid", y = -12,
        w = 240, h = 100,
        bg_color = "#24303c", text_color = "#ffffff",
    })
    back:on("clicked", function()
        caller:load()
        gs:delete()
    end)

    gs:load()
end)

scr:load()
