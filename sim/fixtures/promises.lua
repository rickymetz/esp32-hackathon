-- Documented behaviour that had never actually worked. Each case here is a
-- promise the app contract makes to the five people writing apps; each one
-- failed silently, which is why none of them were noticed.
local lvgl = require("lvgl")
local ui = require("ui")
local timer = require("timer")
lvgl.init({ buffer_lines = 40 })
local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

local function check(name, fn)
    local ok, err = pcall(fn)
    print(string.format("PROM %-44s %s", name, ok and "ok" or ("FAIL " .. tostring(err))))
end

-- ui.row's handle documents get() and set_checked() on EVERY kind. A toggle
-- had get() but a no-op set_checked(); a check had set_checked() but a get()
-- that returned nil. Exact mirror images, both silent.
local list = ui.list(scr, { y = 0, h = 400 })

check("toggle set_checked actually toggles", function()
    local r = ui.row(list, { text = "Toggle", kind = "toggle", checked = false })
    assert(not (r.get() and r.get() ~= 0), "expected off initially")
    r.set_checked(true)
    local v = r.get()
    assert(v and v ~= 0, "set_checked(true) did not take")
    r.set_checked(false)
    v = r.get()
    assert(not (v and v ~= 0), "set_checked(false) did not take")
end)

check("check row get reflects set_checked", function()
    local r = ui.row(list, { text = "Check", kind = "check", checked = true })
    assert(r.get() == true, "expected true from a checked row, got " .. tostring(r.get()))
    r.set_checked(false)
    assert(r.get() == false, "get() did not follow set_checked(false)")
end)

check("nav row still answers both calls", function()
    local r = ui.row(list, { text = "Nav", kind = "nav" })
    r.set_checked(true)          -- meaningless here, must not raise
    local _ = r.get()
end)

-- lvgl.image accepted a card-relative src and drew nothing, because only
-- paths carrying the fs-driver letter open. Both spellings must load the same
-- file, and a missing one must not raise.
--
-- The size assertions run from a timer, not inline: an image takes its size
-- from the decoded header during layout, so reading it in the same breath as
-- creating it reports 0 whether or not the file loaded.
local img_rel  = lvgl.image(scr, { src = "apps/quicktap/icon.bin" })
local img_abs  = lvgl.image(scr, { src = "D:/apps/quicktap/icon.bin" })

check("image with a missing file does not raise", function()
    lvgl.image(scr, { src = "apps/definitely_not_here.bin" })
end)

check("image src may still be a symbol glyph", function()
    lvgl.image(scr, { src = lvgl.symbol.wifi })
end)

scr:load()

timer.after(400, function()
    check("image accepts a card-relative path", function()
        local aw = img_rel:get_size()
        local bw = img_abs:get_size()
        assert(aw == bw, string.format("card-relative gave %s, D: gave %s", tostring(aw), tostring(bw)))
        assert(aw > 0, "icon did not load by either spelling (w=" .. tostring(aw) .. ")")
    end)
    print("PROM done")
end)
