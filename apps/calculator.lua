-- Calculator: a four-function calculator sized for real fingers.
--
-- The keypad is a 4x4 lvgl.buttonmatrix so every digit/operator key clears the
-- 88px touch floor (~86x87px); a 5-row pad would force ~60px rows, squarely in
-- the band CLAUDE.md measured dropping ~half its taps. To fit four rows under a
-- display, C and backspace live as their own buttons in the display strip and
-- the rarely-used % / +- keys are dropped -- negate via "0 - n", and this stays
-- a clean four-function calc.
--
--   * A key commits on release of the cell the finger went DOWN on (pressed
--     captures it, released fires it), so a tap that slides into a neighbour
--     still registers the intended key.
--   * Arithmetic is floating point and integers format with %.0f up to 1e15, so
--     a 10-digit result shows in full instead of hitting the 32-bit integer
--     ceiling; overflow and divide-by-zero both show a sticky "Err" that only C
--     or backspace clears.
--   * A dim "value op" hint shows the pending operation.
--
-- Build the UI and return; there is no loop and no timer.

local lvgl = require("lvgl")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

-- ---- calculator state --------------------------------------------------

local acc = nil        -- the value banked before the pending operator
local op = nil         -- pending operator, or nil
local entry = "0"      -- the number currently being typed / shown
local fresh = true     -- true => the next digit starts a new entry
local err = false      -- true while showing an error; only C / backspace clears

local ENTRY_MAX = 9    -- keep a typed number within the readout width

local function fmt(n)
    if n ~= n or n == math.huge or n == -math.huge then return "Err" end
    -- Test integrality directly rather than via math.tointeger, whose range is
    -- the 32-bit Lua int on the device -- otherwise a 10-digit integer result
    -- would fall to scientific notation and lose digits.
    if n == math.floor(n) and math.abs(n) < 1e15 then
        return string.format("%.0f", n)
    end
    return string.format("%.10g", n)
end

-- ---- widgets -----------------------------------------------------------

local hint = lvgl.label(scr, {
    text = "",
    align = "top_left", x = 186, y = 10,
    text_color = "#7A8699",
    font = lvgl.font(26),
})

local disp = lvgl.label(scr, {
    text = "0",
    align = "top_right", x = -14, y = 22,
    text_color = "#FFFFFF",
    font = lvgl.font(48),
})

local function show()
    disp:set_text(entry)
    disp:set_style({ font = lvgl.font(#entry > 7 and 32 or 48) })
    disp:set_style({ text_color = err and "#EB5757" or "#FFFFFF" })
end

local function show_hint()
    if op and acc then hint:set_text(fmt(acc) .. " " .. op) else hint:set_text("") end
end

local function value()
    -- Force float so device-side 32-bit integer arithmetic cannot wrap.
    return (tonumber(entry) or 0) + 0.0
end

local function result(n)
    entry = fmt(n)
    if entry == "Err" then err = true end
end

local function apply(a, o, b)
    if o == "+" then return a + b end
    if o == "-" then return a - b end
    if o == "x" then return a * b end
    if o == "/" then return b ~= 0 and a / b or (0 / 0) end
    return b
end

local function press(k)
    if err then
        -- Latched error: ignore everything until the user clears it.
        if k == "C" or k == "<" then
            acc, op, entry, fresh, err = nil, nil, "0", true, false
        end
        return
    end

    if k:match("^%d$") then
        if fresh or entry == "0" then entry = k
        elseif #entry < ENTRY_MAX then entry = entry .. k end
        fresh = false
    elseif k == "." then
        if fresh then entry, fresh = "0.", false
        elseif not entry:find("%.") and #entry < ENTRY_MAX then entry = entry .. "." end
    elseif k == "C" then
        acc, op, entry, fresh = nil, nil, "0", true
    elseif k == "<" then                              -- backspace
        if fresh or #entry <= 1 or (entry:sub(1, 1) == "-" and #entry == 2) then
            entry, fresh = "0", true
        else
            entry = entry:sub(1, #entry - 1)
        end
    elseif k == "+" or k == "-" or k == "x" or k == "/" then
        if op and not fresh then
            result(apply(acc, op, value()))
            acc = tonumber(entry) or 0
        else
            acc = value()
        end
        op, fresh = k, true
    elseif k == "=" then
        if op then
            result(apply(acc, op, value()))
            acc, op, fresh = nil, nil, true
        end
    end
end

-- Utility keys live in the display strip so the number pad can stay 4 rows.
local function util(label, x, key)
    local b = lvgl.button(scr, {
        text = label, x = x, y = 6, w = 84, h = 78,
        bg_color = "#24303C", text_color = "#9FB4C7", radius = 12,
    })
    b:on("clicked", function() press(key); show(); show_hint() end)
end
util("C", 8, "C")
util(lvgl.symbol.backspace, 98, "<")

-- ---- keypad (4x4, >=~86px keys) ---------------------------------------

local pad = lvgl.buttonmatrix(scr, {})
pad:set_size(368, 358)
pad:align("bottom_mid", 0, 0)
pad:set_style({ bg_opa = 0, border_width = 0, pad = 4 })
pad:set_map({
    "7", "8", "9", "/", "\n",
    "4", "5", "6", "x", "\n",
    "1", "2", "3", "-", "\n",
    "0", ".", "=", "+",
})

local armed_key
pad:on("pressed", function()
    local i = pad:get_selected()
    armed_key = (i and pad:get_button_text(i)) or nil
end)
pad:on("released", function()
    local k = armed_key
    armed_key = nil
    if not k or k == "" then return end
    press(k)
    show()
    show_hint()
end)

scr:load()
