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
-- Pick a network from a scan; type only the password. Typing an SSID blind is
-- how this used to work, and it made every typo look like a wrong password.
-- Ported here from the retired apps/wifi_setup.lua, which this page absorbed.
local function page_wifi()
    scr:clean()

    local poll      -- declared first: on_back and the closures below all use it
    local scanning = true
    local scan_deferred = false   -- a scan refused with "connecting"; retry later
    local begin_scan

    ui.header(scr, { title = "Wi-Fi", on_back = function()
        -- Cancel on the way out. A live timer whose widgets scr:clean() has
        -- deleted raises on every tick -- logged, not fatal, and it never frees
        -- its slot, so 16 visits exhaust the app's timers.
        if poll then poll:cancel(); poll = nil end
        show_menu()
    end })

    -- Sized from the bottom up, because the buttons are the fixed thing:
    -- buttons occupy 350-438, the status line ~314-348, so the list gets
    -- 96..302. Two 104px rows fit and the rest scrolls -- that is the panel's
    -- constraint, not a layout bug. Do not shrink the rows to fit more.
    local status = ui.note(scr, "", { align = "bottom_mid", y = -100, size = 26 })
    local list = ui.list(scr, { y = 96, h = 206, pad_row = 12 })

    -- Only touch the label when the text actually changes. The poll runs at
    -- 4Hz and most ticks say the same thing, so this is mostly to stop a
    -- steady stream of identical set_text calls on a label that other code
    -- (render) may be rebuilding around.
    local status_text
    local function set_status(t)
        if t ~= status_text then
            status_text = t
            status:set_text(t)
        end
    end

    local function connect_to(ssid, pass)
        -- No prefs.set here. wifi.connect() saves the credentials itself,
        -- from C, into a namespace `prefs` does not expose -- writing them
        -- through prefs as well would put the plaintext password back where
        -- any app could read it.
        set_status("connecting...")
        wifi.connect(ssid, pass or "")
    end

    -- Nothing typed here leaves the board: the on-screen keyboard writes
    -- straight to NVS. A password must never be asked for over the serial
    -- link, which is why this page exists rather than a host-side tool.
    local function ask_password(ssid)
        keyboard.open({ title = ssid, mode = "text" }, function(t)
            if t then connect_to(ssid, t) end
        end)
    end

    -- A scan omits hidden networks, so manual entry has to stay reachable.
    local function manual_entry()
        keyboard.open({ title = "Network", mode = "text" }, function(name)
            if not name or name == "" then return end
            keyboard.open({ title = "Password", mode = "text" }, function(pass)
                connect_to(name, pass or "")
            end)
        end)
    end

    local function render(nets)
        list:clean()
        if nets == nil then
            ui.note(list, "scanning...", { size = 26 })
        elseif #nets == 0 then
            ui.note(list, "no networks found", { size = 26 })
        else
            for _, net in ipairs(nets) do
                -- There is no padlock in the symbol roster (APP_CONTRACT);
                -- eye_close is the nearest "closed" glyph. Swap it if a lock
                -- is ever added to the font.
                local label = net.secure
                    and (lvgl.symbol.eye_close .. "  " .. net.ssid) or net.ssid
                ui.row(list, {
                    text = label, kind = "nav",
                    on_click = function()
                        if net.secure then ask_password(net.ssid)
                        else connect_to(net.ssid, "") end
                    end,
                })
            end
        end
        ui.row(list, { text = "Other network...", kind = "nav", on_click = manual_entry })
    end

    -- scan_start refuses while a connect is in flight, which is exactly the
    -- state during the boot auto-connect -- the common case, not an edge. Two
    -- things must happen on refusal and the first attempt only did the second:
    -- CLEAR THE LIST, because render(nil) has just filled it with
    -- "scanning..." and leaving it there reproduces the very stuck-forever bug
    -- this was meant to fix; and remember to try again, because "connecting"
    -- is temporary and the poll retries once it resolves.
    begin_scan = function()
        scanning = true
        render(nil)
        local ok, err = wifi.scan_start()
        if not ok then
            scanning = false
            scan_deferred = (err == "connecting")
            render({})                      -- "no networks found" + Other...
            set_status("cannot scan - " .. tostring(err))
        end
    end

    ui.button(scr, {
        text = "Rescan", kind = "secondary",
        align = "bottom_left", x = 12, y = -10, w = 164, h = 88,
        on_click = function() begin_scan() end,
    })

    ui.button(scr, {
        text = "Forget", kind = "danger",
        align = "bottom_right", x = -12, y = -10, w = 164, h = 88,
        on_click = function()
            ui.confirm({
                title = "Forget network?",
                message = "The board will stop connecting on its own.",
                confirm_label = "Forget",
                destructive = true,
            }, function(yes)
                if yes then
                    wifi.forget()
                    wifi.disconnect()
                    ui.toast(scr, "forgotten")
                end
            end)
        end,
    })

    begin_scan()

    -- One timer for both jobs. Polls faster than either thing it watches and
    -- repaints only on change: a poll matched to the source's own rate misses
    -- updates (APP_CONTRACT, timer section).
    poll = timer.every(250, function()
        -- A scan refused because a connect was in flight: retry once that
        -- resolves, instead of leaving the user to guess and tap Rescan.
        if scan_deferred and wifi.status() ~= "connecting" then
            scan_deferred = false
            begin_scan()
        end
        if scanning then
            local nets = wifi.scan_results()
            if nets then
                scanning = false
                render(nets)
            end
        end
        local st = wifi.status()
        if st == "connected" then
            set_status(wifi.time_synced() and "connected  clock synced"
                                                or ("connected  " .. (wifi.ip() or "")))
        elseif st == "failed" then
            set_status("failed - " .. (wifi.error() or "check the password"))
        elseif st == "retrying" then
            set_status("retrying - " .. (wifi.error() or "network not found"))
        elseif st == "connecting" then
            set_status("connecting...")
        elseif st == "off" then
            -- Without this the label keeps whatever it last said, so tapping
            -- Forget left "connected  clock synced" on screen -- the exact
            -- state the user had just asked to end.
            set_status("not connected")
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

    -- The developer FPS/CPU readout. Off by default and remembered, so a
    -- release boots clean and turning it on does not need a reflash. It draws
    -- on the display's system layer, so it appears immediately and stays put
    -- across every screen -- including after this app exits, which is the
    -- point: you turn it on here to watch something else.
    -- Blanking is the better default for battery, but a watch you have to
    -- press a button to read is a worse watch -- so it is a choice, which is
    -- what task #40 asked for. Dimming still happens either way at 30s.
    local row_dim
    row_dim = ui.row(list, {
        text = "Never blank", kind = "toggle",
        checked = prefs.get("dim_only", 0) == 1,
        on_change = function()
            local on = row_dim.get()
            prefs.set("dim_only", (on and on ~= 0) and 1 or 0)
        end,
    })

    local row_fps
    row_fps = ui.row(list, {
        text = "FPS overlay", kind = "toggle",
        checked = prefs.get("fps", 0) == 1,
        on_change = function()
            local on = row_fps.get()
            on = on and on ~= 0
            lvgl.perf_overlay(on)
            prefs.set("fps", on and 1 or 0)
        end,
    })
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
