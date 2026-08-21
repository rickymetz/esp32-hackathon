-- Proves the sandbox trim actually sticks. Before the fix, require("debug")
-- handed back the live debug table from the registry's LOADED cache even
-- though the global `debug` was nilled, so this pcall would succeed and
-- sethook(nil) would disable the launcher's interrupt hook -- making the
-- while-true loop below unkillable via STOP. After the fix, require("debug")
-- must not yield a usable sethook, so the interrupt hook stays armed and
-- STOP still works.
print("HOOK_BYPASS starting")

local ok, err = pcall(function()
    require("debug").sethook(nil)
end)

print("HOOK_BYPASS pcall ok=" .. tostring(ok) .. " err=" .. tostring(err))
print("HOOK_BYPASS entering while true")

while true do end
