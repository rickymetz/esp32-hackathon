-- Tip -- split a bill. Tap the amount to key it in, set tip % and party size
-- with the steppers, and read the total and per-person share.
--
-- The number pad returns digits; we read them as cents (key 4250 -> $42.50),
-- so no decimal point is needed.
--
-- Install: ./.venv/bin/python tools/push.py apps/tip.lua

local lvgl = require("lvgl")
local ui = require("ui")
local keyboard = require("keyboard")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

ui.title(scr, "Tip")

local bill_cents = 0
local tip_pct = 18
local people = 1

local list = lvgl.container(scr, {
    x = 0, y = 68, w = 368, h = 372, bg_opa = 0, border_width = 0, pad = 12,
})
list:set_flex({ flow = "column", pad_row = 12 })
list:set_scroll({ scrollbar = "active" })

local function dollars(cents) return string.format("$%.2f", cents / 100) end

local bill_row, result
local function recompute()
    local total = bill_cents * (100 + tip_pct) / 100
    local each = total / people
    -- "Each" first and larger -- the per-person amount is the answer.
    result:set_text(string.format("Each %s\nTotal %s", dollars(each), dollars(total)))
end

bill_row = ui.row(list, {
    text = "Bill: $0.00", kind = "nav",
    on_click = function()
        keyboard.open({ title = "Bill (cents)", mode = "number",
                        initial = tostring(bill_cents) }, function(t)
            if t then
                bill_cents = math.floor(tonumber(t) or 0)
                bill_row.label:set_text("Bill: " .. dollars(bill_cents))
                recompute()
            end
        end)
    end,
})

ui.stepper(list, { min = 0, max = 30, step = 1, value = tip_pct, label = "%d%% tip" },
    function(v) tip_pct = v; recompute() end)

ui.stepper(list, { min = 1, max = 12, step = 1, value = people, label = "%d ppl" },
    function(v) people = v; recompute() end)

-- Result is the last item in the scrollable list, so it never overlaps the
-- steppers above it.
result = lvgl.label(list, {
    text = "Each $0.00\nTotal $0.00", text_color = "#ffffff",
})
result:set_style({ font = lvgl.font(40) })

scr:load()
