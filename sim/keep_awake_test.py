#!/usr/bin/env python3
"""Contract test for lvgl.keep_awake().

The flag itself does nothing in the simulator -- only the launcher's poll loop
reads it -- so this pins the LUA CONTRACT: default off, setter returns the new
value, reads are stable, and the function exists at all.

That last part is the one that matters most. keep_awake lives in the shared
binding (lua_lvgl_runtime.c, which sim/CMakeLists.txt compiles as-is) precisely
so the sim cannot drift out of sync with the device the way a hand-written stub
would. This test fails loudly if it is ever moved somewhere only the device
compiles.

    sim/keep_awake_test.py      # exits non-zero on failure
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SIM = os.path.join(HERE, "build", "sim")
FIXTURE = os.path.join("sim", "fixtures", "keep_awake.lua")

if not os.path.exists(SIM):
    sys.exit("sim not built -- run sim/build.sh")

out = subprocess.run([SIM, "--sdroot", REPO, "run", FIXTURE, ":", "sleep", "1"],
                     cwd=REPO, capture_output=True, text=True, timeout=60).stdout

expected = [
    (r"DEFAULT false",   "keep_awake() must default to false -- a device that "
                         "boots pinned awake is the bug this feature prevents"),
    (r"SET_TRUE true",   "keep_awake(true) must return the new state"),
    (r"READBACK true",   "the flag must persist between calls"),
    (r"SET_FALSE false", "keep_awake(false) must return the new state"),
    (r"READBACK2 false", "clearing the flag must stick"),
    (r"KADONE",          "fixture never completed -- keep_awake is probably nil"),
]

fails = [why for pat, why in expected if not re.search(pat, out)]

if fails:
    print(out.strip()[-400:])
    for f in fails:
        print("FAIL:", f)
    sys.exit(1)
print("keep_awake: ok")
