-- Calculator: a four-function calculator built on lvgl.buttonmatrix -- the
-- widget for a dense keypad, since it hit-tests one grid internally instead of
-- tiling many separate button widgets (which would leave dead gaps between
-- keys). At five rows under the display the keys land ~86x67px, under the
-- contract's 88x88 touch floor: a deliberate density tradeoff a calculator
-- forces on a 448px screen -- fewer, larger keys couldn't hold a full keypad.
--
--   * A key commits on release of the cell the finger went DOWN on (pressed
--     captures it, released fires it), so a sloppy tap that slides into a
--     neighbour still registers the intended key -- value_changed alone fires
--     per-cell while dragging and would enter several.
--   * A dim hint shows the pending "value op" so the armed operation is visible.
--   * Arithmetic runs in floating point: 32-bit Lua integers would wrap on a
--     large product with no error. Results format as an integer when exact and
--     in range, else %.6g -- so "6 / 2" reads "3", a huge product reads in
--     scientific rather than a wrapped negative.
--
-- Build the UI and return; there is no loop and no timer.

local lvgl = require("lvgl")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

-- A dim readout of the pending "bankedValue operator", top-left, so the user
-- can see which operation is armed while typing the second operand.
local hint = lvgl.label(scr, {
    text = "",
    align = "top_left", x = 18, y = 34,
    text_color = "#7A8699",
    font = lvgl.font(26),
})

-- Display: right-aligned so digits grow leftward from the edge.
local disp = lvgl.label(scr, {
    text = "0",
    align = "top_right", x = -18, y = 26,
    text_color = "#FFFFFF",
    font = lvgl.font(60),
})

-- ---- calculator state --------------------------------------------------

local acc = nil        -- the value banked before the pending operator
local op = nil         -- pending operator, or nil
local entry = "0"      -- the number currently being typed / shown
local fresh = true     -- true => the next digit starts a new entry

local ENTRY_MAX = 12   -- keep the readout within the panel width

local function fmt(n)
    if n ~= n then return "Err" end          -- NaN (guarded 0/0)
    local i = math.tointeger(n)              -- nil if not exact or out of 32-bit range
    if i then return tostring(i) end
    return string.format("%.6g", n)
end

local function show()
    disp:set_text(entry)
end

local function show_hint()
    if op and acc then hint:set_text(fmt(acc) .. " " .. op) else hint:set_text("") end
end

local function value()
    -- Force float so device-side 32-bit integer arithmetic cannot wrap.
    return (tonumber(entry) or 0) + 0.0
end

local function apply(a, o, b)
    if o == "+" then return a + b end
    if o == "-" then return a - b end
    if o == "x" then return a * b end
    if o == "/" then return b ~= 0 and a / b or (0 / 0) end
    return b
end

local function press(k)
    if k:match("^%d$") then
        if fresh or entry == "0" then entry = k
        elseif #entry < ENTRY_MAX then entry = entry .. k end
        fresh = false
    elseif k == "." then
        if fresh then entry, fresh = "0.", false
        elseif not entry:find("%.") and #entry < ENTRY_MAX then entry = entry .. "." end
    elseif k == "C" then
        acc, op, entry, fresh = nil, nil, "0", true
    elseif k == "+/-" then
        if entry:sub(1, 1) == "-" then entry = entry:sub(2)
        elseif entry ~= "0" then entry = "-" .. entry end
    elseif k == "%" then
        entry, fresh = fmt(value() / 100), true
    elseif k == "+" or k == "-" or k == "x" or k == "/" then
        if op and not fresh then
            acc = apply(acc, op, value())
            entry = fmt(acc)
        else
            acc = value()
        end
        op, fresh = k, true
    elseif k == "=" then
        if op then
            entry = fmt(apply(acc, op, value()))
            acc, op, fresh = nil, nil, true
        end
    end
end

-- ---- keypad ------------------------------------------------------------

local pad = lvgl.buttonmatrix(scr, {})
pad:set_size(368, 336)
pad:align("bottom_mid", 0, 0)
pad:set_style({ bg_opa = 0, border_width = 0, pad = 6 })
pad:set_map({
    "C",   "+/-", "%",   "/", "\n",
    "7",   "8",   "9",   "x", "\n",
    "4",   "5",   "6",   "-", "\n",
    "1",   "2",   "3",   "+", "\n",
    "0",   ".",   "=",
})

-- Commit the key the finger went DOWN on, on release -- see the header note.
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
