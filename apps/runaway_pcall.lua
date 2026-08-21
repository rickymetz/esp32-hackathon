-- A defensive author wraps their loop. The hook's error must not be swallowed.
print("RUNAWAY pcall starting")
while true do
    pcall(function() while true do end end)
end
