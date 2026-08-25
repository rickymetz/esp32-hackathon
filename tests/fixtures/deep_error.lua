-- Regression fixture for a deep traceback. Six nested (non-tail) calls deep
-- before the error, so the error screen's body label has enough stack
-- frames to overflow a one-line-tall box and prove the scrollable/
-- height-capped fix. Each wrapper stores the call result in a local before
-- returning it -- returning the call directly would tail-call-optimize
-- and Lua would collapse the frames into "(...tail calls...)" instead of
-- keeping them as separate traceback lines.

local function level6()
    local t = nil
    return t.field   -- attempt to index a nil value
end

local function level5() local r = level6(); return r end
local function level4() local r = level5(); return r end
local function level3() local r = level4(); return r end
local function level2() local r = level3(); return r end
local function level1() local r = level2(); return r end

level1()
