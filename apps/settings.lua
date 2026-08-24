-- Settings -- device preferences, in one place.
--
-- Owns the four things that make this feel like a watch you set up rather
-- than a dev board: the watch face, Wi-Fi, date & time, and display/sound.
-- It absorbs what used to be a separate wifi_setup app.
--
-- Everything here writes `prefs`, which is NVS, NOT the SD card. That is the
-- point: these are device settings and must survive with no card in the slot,
-- and the C shell (the watch face) reads the same keys. `store` is for
-- per-app state on the card; `prefs` is for the device.

local lvgl     = require("lvgl")
local ui       = require("ui")
local prefs    = require("prefs")
local wifi     = require("wifi")
local rtc      = require("rtc")
local audio    = require("audio")
local timer    = require("timer")
local keyboard = require("keyboard")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

-- Face style names, in the order of launcher_face_style_t in
-- launcher/main/launcher_face.h. The shell reads the index back as that enum,
-- so this list must stay in that order -- adding a face means adding it here
-- too. (Kept in Lua rather than bound from C because it is five strings; if it
-- grows, bind launcher_face_style_name() instead of letting these drift.)
local FACES = { "Digital", "Analog", "Rings", "Words", "Minimal" }

-- Plain "..." rather than the ellipsis character: Lexend has no U+2026 glyph,
-- so it renders as an empty box (caught in the simulator, same class of bug as
-- the 120px face's missing "-").

local show_menu   -- forward declaration; each sub-screen returns here

-- ---------------------------------------------------------------- watch face
local function page_face()
    ui.picker({ title = "Watch face", options = FACES,
                selected = prefs.get("face", 0) + 1 }, function(i)
        if i then prefs.set("face", i - 1) end   -- 0-based in C
        show_menu()
    end)
end

-- --------------------------------------------------------------------- Wi-Fi
local function page_wifi()
    scr:clean()
    ui.header(scr, { title = "Wi-Fi", on_back = show_menu })

    local ssid = prefs.get("wifi_ssid", "")
    local pass = prefs.get("wifi_pass", "")

    local status = ui.note(scr, "", { y = 150, size = 26 })
    local list = ui.list(scr, { y = 96, h = 210, pad_row = 12 })

    local row_ssid = ui.row(list, { text = ssid ~= "" and ssid or "Network...", kind = "nav" })
    local row_pass = ui.row(list, { text = pass ~= "" and "Password set" or "Password...", kind = "nav" })

    -- Nothing typed here ever leaves the board: the on-screen keyboard writes
    -- straight to prefs. A password must never be asked for over the serial
    -- link, which is why this app exists rather than a host-side tool.
    row_ssid.row:on("clicked", function()
        keyboard.open({ title = "Network", mode = "text", initial = ssid }, function(t)
            if t then ssid = t; prefs.set("wifi_ssid", t); row_ssid.label:set_text(t) end
        end)
    end)
    row_pass.row:on("clicked", function()
        keyboard.open({ title = "Password", mode = "text", initial = pass }, function(t)
            if t then pass = t; prefs.set("wifi_pass", t); row_pass.label:set_text("Password set") end
        end)
    end)

    ui.button(scr, { text = "Connect", kind = "primary", align = "bottom_mid", y = -8,
                     w = 344, h = 96,
        on_click = function()
            if ssid == "" then status:set_text("Enter a network first"); return end
            status:set_text("connecting...")
            wifi.connect(ssid, pass)
        end })

    -- wifi.connect() never blocks -- it starts the attempt and returns -- so
    -- the result has to be polled. "failed" means five attempts, usually a
    -- wrong password.
    timer.every(500, function()
        local st = wifi.status()
        if st == "connected" then
            status:set_text(wifi.time_synced() and ("connected  clock synced")
                                               or ("connected  " .. (wifi.ip() or "")))
        elseif st == "failed" then
            status:set_text("failed - check the password")
        elseif st == "connecting" then
            status:set_text("connecting...")
        end
    end)
end

-- --------------------------------------------------------------- date & time
local function page_time()
    scr:clean()
    ui.header(scr, { title = "Date & time", on_back = show_menu })

    local list = ui.list(scr, { y = 96, h = 250, pad_row = 12 })

    -- Three keys, one writer. The C watch face consumes only tz_min and
    -- applies minutes-east directly -- it has no city table and no DST rules,
    -- so tz_min must already be the EFFECTIVE offset. City and DST are stored
    -- alongside it purely so this page can show what was chosen: tz_min alone
    -- cannot be reversed into a city once DST is folded in (New York in summer
    -- is -240, which is also Santiago).
    local zone_idx = prefs.get("tz_city", 11)          -- 11 = London = UTC
    if zone_idx < 1 or zone_idx > #ui.ZONES then zone_idx = 11 end
    local dst = prefs.get("tz_dst", 0) == 1

    local function write_zone(idx, on)
        prefs.set("tz_city", idx)
        prefs.set("tz_dst", on and 1 or 0)
        prefs.set("tz_min", ui.ZONES[idx][2] + (on and 60 or 0))
    end

    local row_zone = ui.row(list, { text = "Zone: " .. ui.ZONES[zone_idx][1], kind = "nav" })
    row_zone.row:on("clicked", function()
        local names = {}
        for i, z in ipairs(ui.ZONES) do names[i] = z[1] end
        ui.picker({ title = "Time zone", options = names, selected = zone_idx, size = 26 }, function(i)
            if i then
                write_zone(i, dst)
            end
            page_time()
        end)
    end)

    -- Summer time is a manual toggle, not a rules table: DST boundaries differ
    -- per country and change by legislation, so a baked-in table would go
    -- quietly wrong. One switch the user flips twice a year is honest.
    local row_dst
    row_dst = ui.row(list, {
        text = "Summer time", kind = "toggle", checked = dst,
        on_change = function()
            local on = row_dst.get()
            write_zone(zone_idx, on and on ~= 0)
            page_time()
        end,
    })

    -- Read-only status: NTP is the intended way to set the clock, so say
    -- plainly whether it has happened rather than offering a manual entry
    -- path that would immediately be overwritten by the next sync.
    local t = rtc.now()
    ui.row(list, { text = t and string.format("%04d-%02d-%02d  %02d:%02d",
                                              t.year, t.month, t.day, t.hour, t.min)
                          or "Clock not set", dim = true })
    ui.row(list, { text = wifi.time_synced() and "Synced over Wi-Fi this boot"
                                             or "Not synced - connect Wi-Fi", dim = true })

    -- ui.note wraps to its own width now, so the line break that used to be
    -- hand-placed here (it did not, and a long line ran off both edges) would
    -- only force an awkward break at a larger font scale.
    ui.note(scr, "Clock keeps UTC. The zone shifts the display.",
            { align = "bottom_mid", y = -8, size = 26 })
end

-- ----------------------------------------------------------- display & sound
local function page_display()
    scr:clean()
    ui.header(scr, { title = "Display & sound", on_back = show_menu })

    local list = ui.list(scr, { y = 96, h = 300, pad_row = 16 })

    -- Font scale drives every lvgl.font() and the theme default, so the
    -- preview below re-styles immediately while the launcher picks it up on
    -- exit (it re-applies the theme then).
    local pct = math.floor(lvgl.font_scale() * 100 + 0.5)
    ui.stepper(list, { min = 70, max = 130, step = 10, value = pct, label = "Text %d%%" },
        function(v)
            lvgl.font_scale(v / 100)
            prefs.set("font_pct", v)
        end)

    local vol = audio.volume() or 70
    ui.stepper(list, { min = 0, max = 100, step = 10, value = vol, label = "Volume %d%%" },
        function(v)
            audio.volume(v)
            prefs.set("volume", v)
            audio.beep()      -- hear what you just set
        end)
end

-- --------------------------------------------------------------------- about
local function page_about()
    scr:clean()
    ui.header(scr, { title = "About", on_back = show_menu })

    local list = ui.list(scr, { y = 96, h = 320, pad_row = 12 })
    ui.row(list, { text = "Waveshare S3 AMOLED 1.8", dim = true })
    ui.row(list, { text = "368x448 - 8 MB PSRAM", dim = true })

    local ip = wifi.ip()
    ui.row(list, { text = ip and ("IP " .. ip) or "Not on a network", dim = true })

    -- Whether the card is present matters here more than anywhere: without
    -- it there are no apps, and this is where someone will come to find out.
    local probe = io.open("/sdcard/apps", "r")
    if probe then probe:close() end
    ui.row(list, { text = probe and "SD card present" or "No SD card", dim = true })
end

-- ----------------------------------------------------------------- main menu
show_menu = function()
    scr:clean()
    ui.title(scr, "Settings")

    local list = ui.list(scr, { y = 90, h = 358, pad_row = 12 })
    local items = {
        { lvgl.symbol.clock    .. "  Watch face",      page_face },
        { lvgl.symbol.wifi     .. "  Wi-Fi",           page_wifi },
        { lvgl.symbol.calendar .. "  Date & time",     page_time },
        { lvgl.symbol.settings .. "  Display & sound", page_display },
        { lvgl.symbol.list     .. "  About",           page_about },
    }
    for _, it in ipairs(items) do
        ui.row(list, { text = it[1], kind = "nav", on_click = it[2] })
    end
end

show_menu()
scr:load()
