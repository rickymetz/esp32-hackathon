-- Level -- a bubble level from the accelerometer.
--
-- Reads imu.accel() every 60ms and floats a bubble the way a spirit level
-- does: centred when the board is flat, drifting toward the raised side as you
-- tilt. The accelerometer is the right sensor for this -- gravity is a stable
-- reference and the contract flags it good to ~10%, while the gyro is only a
-- motion detector. When the tilt drops under ~2 degrees the bubble turns green
-- and the readout says LEVEL.
--
-- In the simulator the IMU reads dead-flat (accel (0,0,1)g), so this shows the
-- centred, level state; on hardware the bubble tracks real tilt.

local lvgl = require("lvgl")
local imu = require("imu")
local timer = require("timer")
local ui = require("ui")

lvgl.init({ buffer_lines = 40 })

local CX, CY   = 184, 214       -- vial centre on the 368x448 panel
local R_VIAL   = 140            -- outer vial radius (clears the hero readout)
local R_BUBBLE = 24             -- bubble radius
local TRAVEL   = R_VIAL - R_BUBBLE
local GAIN     = TRAVEL / 0.5   -- 0.5 g (~30 deg) sends the bubble to the rim
local LEVEL_DEG = 2.0           -- within this, call it level

-- A spirit-level bubble floats toward the RAISED side, i.e. opposite the
-- in-plane gravity component, so the offset is negated. Which physical edge
-- that is depends on the QMI8658's mounting sign, which can't be checked in the
-- sim (it reads dead-flat); flip this to +1 if hardware shows it tracking the
-- low side instead.
local SIGN = -1

local GREEN = "#39D98A"
local AMBER = "#F2C94C"
local TRACK = "#2A2A33"

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

ui.title(scr, "Level")

-- The vial: an outlined circle with a faint centre target and crosshair.
local vial = lvgl.container(scr, {
    align = "center", y = CY - 224,
    w = R_VIAL * 2, h = R_VIAL * 2,
    bg_opa = 0, radius = R_VIAL,
    border_color = TRACK, border_width = 4,
})
vial:set_clickable(false)

local target = lvgl.container(scr, {
    align = "center", y = CY - 224,
    w = (R_BUBBLE + 8) * 2, h = (R_BUBBLE + 8) * 2,
    bg_opa = 0, radius = R_BUBBLE + 8,
    border_color = TRACK, border_width = 2,
})
target:set_clickable(false)

lvgl.line(scr, { x = 0, y = 0, w = 368, h = 448, line_color = TRACK, line_width = 2,
                 points = { { x = CX - R_VIAL, y = CY }, { x = CX + R_VIAL, y = CY } } })
lvgl.line(scr, { x = 0, y = 0, w = 368, h = 448, line_color = TRACK, line_width = 2,
                 points = { { x = CX, y = CY - R_VIAL }, { x = CX, y = CY + R_VIAL } } })

local bubble = lvgl.container(scr, {
    align = "center", y = CY - 224,
    w = R_BUBBLE * 2, h = R_BUBBLE * 2,
    bg_color = GREEN, bg_opa = 255, radius = R_BUBBLE, border_width = 0,
})
bubble:set_clickable(false)

local readout = lvgl.label(scr, {
    text = "--", align = "bottom_mid", y = -24,
    text_color = "#FFFFFF", font = lvgl.font(60),
})

local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

local last_level = nil          -- track state so styles are only rewritten on change

local function update()
    local ax, ay, az = imu.accel()
    if not ax then
        readout:set_text("no imu")
        return
    end

    local dx = SIGN * ax * GAIN
    local dy = SIGN * ay * GAIN
    -- Clamp the VECTOR, not each axis: independent per-axis clamping would let
    -- a diagonal tilt push the bubble to TRAVEL*sqrt(2), outside the round vial.
    local d = math.sqrt(dx * dx + dy * dy)
    if d > TRAVEL then
        dx = dx * TRAVEL / d
        dy = dy * TRAVEL / d
    end
    -- align offset is measured from the vial centre (y already at CY-224).
    bubble:align("center", math.floor(dx + 0.5), (CY - 224) + math.floor(dy + 0.5))

    local mag = math.sqrt(ax * ax + ay * ay + az * az)
    local tilt = 0.0
    if mag > 0 then
        tilt = math.deg(math.acos(clamp(az / mag, -1.0, 1.0)))
    end

    local level = tilt <= LEVEL_DEG
    if level ~= last_level then          -- only restyle on a state change
        last_level = level
        bubble:set_style({ bg_color = level and GREEN or AMBER })
        readout:set_style({ text_color = level and GREEN or "#FFFFFF" })
    end
    readout:set_text(level and "LEVEL" or string.format("%.1f\u{00B0}", tilt))
end

update()
timer.every(60, update)

scr:load()
