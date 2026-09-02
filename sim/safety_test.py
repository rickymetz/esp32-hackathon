#!/usr/bin/env python3
"""Memory-safety regressions: every case must raise cleanly, not corrupt.

Each was found by an adversarial review and each was reachable from ordinary
app Lua: resizing a built-in font wrote to .rodata, a chart series used with
the wrong chart gave a controlled out-of-bounds heap write, and a collected
widget handle left the C record pointing into freed Lua heap.

They fail LOUDLY here and silently on hardware, which is the whole point of
running them rather than reasoning about the pointers.

    sim/safety_test.py
"""
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SIM = os.path.join(HERE, "build", "sim")
FIXTURE = os.path.join("sim", "fixtures", "safety.lua")

if not os.path.exists(SIM):
    sys.exit("sim not built -- run sim/build.sh")

out = subprocess.run([SIM, "--sdroot", REPO, "run", FIXTURE, ":", "sleep", "1.5"],
                     cwd=REPO, capture_output=True, text=True, timeout=90).stdout

lines = [l for l in out.splitlines() if l.startswith("SAFE ")]
if "SAFE done" not in out:
    print(out[-2000:])
    sys.exit("fixture did not run to completion")

fails = [l for l in lines if " ok" not in l and "done" not in l]
checks = [l for l in lines if l.strip() != "SAFE done"]
print(f"{len(checks)} memory-safety cases exercised")
for f in fails:
    print("FAIL:", f)
if fails:
    sys.exit(1)
print("safety: ok")
