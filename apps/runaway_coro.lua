-- Coroutines start with hookmask == 0, so a hook on the main thread misses this.
print("RUNAWAY coroutine starting")
local co = coroutine.wrap(function() while true do end end)
co()
