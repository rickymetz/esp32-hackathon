-- store: per-app persistent key/value, saved as JSON on the SD card.
--
--   local store = require("store")
--   local best = store.get("best", 0)      -- default if never saved
--   store.set("best", score)               -- in memory
--   store.save()                           -- write the file
--
-- The launcher injects this app's state-file path; get/set work in memory and
-- save() is what actually touches the card, so a tight loop can set() freely
-- and persist once at a natural moment (game over, item added). One JSON file
-- per app, human-readable. Values may be strings, numbers, booleans, and
-- (nested) tables/arrays -- anything JSON can hold.
--
-- If you forget save(), the LAUNCHER writes it for you when your app exits.
-- BOOT can land at any moment and there is no on_exit hook to save from, so
-- "set it and never save" used to mean silent data loss on the way out. See
-- __flush_if_dirty at the bottom -- it is called from C, not from Lua.

local M = {}

-- ---- minimal JSON --------------------------------------------------------

local function esc(s)
    return (s:gsub('[%z\1-\31\\"]', function(c)
        local m = { ['"'] = '\\"', ['\\'] = '\\\\', ['\n'] = '\\n',
                    ['\r'] = '\\r', ['\t'] = '\\t', ['\b'] = '\\b', ['\f'] = '\\f' }
        return m[c] or string.format('\\u%04x', c:byte())
    end))
end

local function encode(v)
    local t = type(v)
    if t == "nil" then
        return "null"
    elseif t == "boolean" then
        return v and "true" or "false"
    elseif t == "number" then
        if v ~= v or v == math.huge or v == -math.huge then return "null" end
        if math.type(v) == "integer" then return string.format("%d", v) end
        return string.format("%.14g", v)
    elseif t == "string" then
        return '"' .. esc(v) .. '"'
    elseif t == "table" then
        -- Array if keys are 1..n contiguous; else object.
        local n = 0
        for _ in pairs(v) do n = n + 1 end
        local len = #v
        if len == n then
            local parts = {}
            for i = 1, len do parts[i] = encode(v[i]) end
            return "[" .. table.concat(parts, ",") .. "]"
        end
        local parts = {}
        for k, val in pairs(v) do
            parts[#parts + 1] = '"' .. esc(tostring(k)) .. '":' .. encode(val)
        end
        return "{" .. table.concat(parts, ",") .. "}"
    end
    return "null"
end

local function decode(s)
    local i = 1
    local function ws() i = s:find("[^ \t\r\n]", i) or (#s + 1) end
    local value                                        -- forward declaration

    local function str()
        i = i + 1                                      -- opening quote
        local out = {}
        while i <= #s do
            local c = s:sub(i, i)
            if c == '"' then i = i + 1; return table.concat(out) end
            if c == "\\" then
                local e = s:sub(i + 1, i + 1)
                local m = { ['"'] = '"', ['\\'] = '\\', ['/'] = '/', n = '\n',
                            r = '\r', t = '\t', b = '\b', f = '\f' }
                if e == "u" then
                    out[#out + 1] = utf8.char(tonumber(s:sub(i + 2, i + 5), 16) or 0)
                    i = i + 6
                else
                    out[#out + 1] = m[e] or e
                    i = i + 2
                end
            else
                out[#out + 1] = c
                i = i + 1
            end
        end
        error("unterminated string")
    end

    function value()
        ws()
        local c = s:sub(i, i)
        if c == '"' then
            return str()
        elseif c == "{" then
            local o = {}
            i = i + 1; ws()
            if s:sub(i, i) == "}" then i = i + 1; return o end
            while true do
                ws()
                local k = str()
                ws(); i = i + 1                         -- ':'
                o[k] = value()
                ws()
                local d = s:sub(i, i); i = i + 1
                if d == "}" then return o end
                if d ~= "," then error("expected , or }") end
            end
        elseif c == "[" then
            local a = {}
            i = i + 1; ws()
            if s:sub(i, i) == "]" then i = i + 1; return a end
            while true do
                -- A JSON null element collapses: Lua arrays cannot hold a nil
                -- hole, so a[#a+1]=nil is a no-op and the next value takes that
                -- slot. Only reachable if a persisted array held a non-finite
                -- number (NaN/Inf), which encode() writes as null -- don't put
                -- those in a stored array.
                a[#a + 1] = value()
                ws()
                local d = s:sub(i, i); i = i + 1
                if d == "]" then return a end
                if d ~= "," then error("expected , or ]") end
            end
        elseif s:sub(i, i + 3) == "true" then
            i = i + 4; return true
        elseif s:sub(i, i + 4) == "false" then
            i = i + 5; return false
        elseif s:sub(i, i + 3) == "null" then
            i = i + 4; return nil
        else
            local num = s:match("^%-?%d+%.?%d*[eE]?[%+%-]?%d*", i)
            if not num or num == "" then error("unexpected char at " .. i) end
            i = i + #num
            return tonumber(num)
        end
    end

    return value()
end

-- ---- store ---------------------------------------------------------------

local data                                             -- loaded lazily
local dirty = false                                    -- set since the last save?

local function path()
    return rawget(_G, "__APP_STORE__")                 -- injected by the launcher
end

local function load()
    if data then return end
    data = {}
    local p = path()
    if not p then return end
    local f = io.open(p, "r")
    if not f then return end
    local s = f:read("*a")
    f:close()
    if s and s ~= "" then
        local ok, t = pcall(decode, s)
        if ok and type(t) == "table" then data = t end
    end
end

-- Read a value, returning `default` when the key was never stored.
function M.get(key, default)
    load()
    local v = data[key]
    if v == nil then return default end
    return v
end

-- Set a value in memory. Call save() to persist -- or don't, and the launcher
-- writes it when the app exits.
function M.set(key, value)
    load()
    data[key] = value
    dirty = true
end

-- The whole state table, to read or mutate directly (call save() after).
--
-- Marks the store dirty unconditionally: the caller gets a live reference and
-- may mutate it without going through set(), and there is no way to tell
-- afterwards whether they did. Erring toward one unnecessary write on exit is
-- the right side to be wrong on when the alternative is losing the change.
function M.all()
    load()
    dirty = true
    return data
end

-- Forget everything (call save() to persist the empty state).
function M.clear()
    data = {}
    dirty = true
end

-- Write the state file. Returns true, or nil + reason (no path / can't open).
function M.save()
    load()
    local p = path()
    if not p then return nil, "no store path" end
    local f = io.open(p, "w")
    if not f then return nil, "cannot open " .. p end
    f:write(encode(data))
    f:close()
    dirty = false
    return true
end

-- Called by the LAUNCHER from C on app exit, never by an app -- hence the
-- underscores. Returns true if it actually wrote something.
--
-- This is the whole reason an app does not need an on_exit hook to keep its
-- state. BOOT lands whenever the user presses it, including mid-callback, so
-- asking every app to save() on every mutation was a rule that could only be
-- followed perfectly or not at all. The launcher clears its interrupt hook,
-- calls this in a protected call, and restores the stop flag afterwards; an
-- app that has already save()d costs nothing here.
function M.__flush_if_dirty()
    if not dirty then return false end
    local ok = M.save()
    return ok and true or false
end

return M
