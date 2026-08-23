-- Calculator: a four-function calculator built on lvgl.buttonmatrix -- the
-- right widget for a dense keypad, since it hit-tests one grid internally
-- instead of tiling many small button widgets the digitizer would miss.
--
--   * value_changed fires on both press (a real key) and release
--     (get_selected() -> nil); the nil guard drops the release.
--   * two-operand immediate logic: an operator key flushes any pending
--     operation so "2 + 3 + 4" chains without an explicit "=".
--   * hero display via lvgl.font(60); results format as an integer when
--     exact, so "6 ÷ 2" reads "3", not "3.0".
--
-- Build the UI and return; there is no loop and no timer -- the keypad
-- callback does all the work.

local lvgl = require("lvgl")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

-- Display: right-aligned so digits grow leftward from the edge, like a
-- real calculator readout.
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

local function show()
    -- Keep the readout from overflowing the panel width.
    local s = entry
    if #s > 10 then s = s:sub(1, 10) end
    disp:set_text(s)
end

local function value()
    return tonumber(entry) or 0
end

local function fmt(n)
    if n ~= n then return "Err" end          -- NaN (e.g. 0 ÷ 0 guarded below)
    local i = math.tointeger(n)
    if i then return tostring(i) end
    return string.format("%.6g", n)
end

-- Keys use ASCII "x" / "/" for multiply/divide: the compiled Lexend subset
-- covers ASCII 32-126 but not U+00D7/U+00F7, which render as tofu boxes.
local function apply(a, o, b)
    if o == "+" then return a + b end
    if o == "-" then return a - b end
    if o == "x" then return a * b end
    if o == "/" then return b ~= 0 and a / b or (0 / 0) end
    return b
end

local function press(k)
    if k:match("^%d$") then
        if fresh or entry == "0" then entry = k else entry = entry .. k end
        fresh = false
    elseif k == "." then
        if fresh then entry, fresh = "0.", false
        elseif not entry:find("%.", 1, true) then entry = entry .. "." end
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

pad:on("value_changed", function()
    local i = pad:get_selected()
    if not i then return end                 -- release, not a key press
    local k = pad:get_button_text(i)
    if not k or k == "" then return end
    press(k)
    show()
end)

scr:load()
