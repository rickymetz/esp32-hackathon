-- Verifies app_sandbox_apply() removed the dangerous stdlib entries.
print("TRIM debug=" .. tostring(debug))
print("TRIM package=" .. tostring(package))
print("TRIM os.exit=" .. tostring(os.exit))
print("TRIM os.execute=" .. tostring(os.execute))
print("TRIM os.time=" .. tostring(os.time))
print("TRIM coroutine=" .. tostring(coroutine))
