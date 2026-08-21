-- Verifies app_sandbox_apply() removed the dangerous stdlib entries -- and
-- that require() cannot hand them back via the registry cache, and still
-- works for legitimate modules like lvgl/timer.
print("TRIM debug=" .. tostring(debug))
print("TRIM package=" .. tostring(package))
print("TRIM os.exit=" .. tostring(os.exit))
print("TRIM os.execute=" .. tostring(os.execute))
print("TRIM os.time=" .. tostring(os.time))
print("TRIM coroutine=" .. tostring(coroutine))

-- require("debug") must not hand back a usable sethook.
local ok, result = pcall(function() return require("debug") end)
if not ok then
    print("REQUIRE-DEBUG errored: " .. tostring(result))
else
    print("REQUIRE-DEBUG returned: " .. tostring(result))
    if type(result) == "table" then
        print("REQUIRE-DEBUG sethook=" .. tostring(result.sethook))
        print("REQUIRE-DEBUG gethook=" .. tostring(result.gethook))
    end
end

-- require("package") must not hand back a usable loadlib.
local ok2, result2 = pcall(function() return require("package") end)
if not ok2 then
    print("REQUIRE-PACKAGE errored: " .. tostring(result2))
else
    print("REQUIRE-PACKAGE returned: " .. tostring(result2))
    if type(result2) == "table" then
        print("REQUIRE-PACKAGE loadlib=" .. tostring(result2.loadlib))
    end
end

-- Proof require() still works for legitimate capability modules.
local ok3, lvgl = pcall(require, "lvgl")
print("REQUIRE-LVGL ok=" .. tostring(ok3) .. " value=" .. tostring(lvgl))

local ok4, timer = pcall(require, "timer")
print("REQUIRE-TIMER ok=" .. tostring(ok4) .. " value=" .. tostring(timer))
