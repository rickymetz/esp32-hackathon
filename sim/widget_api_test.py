#!/usr/bin/env python3
"""Execute every data-widget method the contract documents.

The contract listed chart/table/list/tabview/msgbox/spinbox/led/buttonmatrix/
calendar/canvas/window as "also available" and documented none of their
methods, so an app author could create one of these and then had no way to
put anything in it short of reading C.

Documenting them from static analysis got 41 of 42 signatures right; the
one that was wrong (calendar:set_highlighted takes POSITIONAL {year,month,
day} arrays, not keyed tables) is exactly why this runs the calls instead
of trusting the prose. Any documented signature that stops matching the
binding fails here.

    sim/widget_api_test.py
"""
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SIM = os.path.join(HERE, "build", "sim")
FIXTURE = os.path.join("sim", "fixtures", "widget_api.lua")

if not os.path.exists(SIM):
    sys.exit("sim not built -- run sim/build.sh")

out = subprocess.run([SIM, "--sdroot", REPO, "run", FIXTURE, ":", "sleep", "1.5"],
                     cwd=REPO, capture_output=True, text=True, timeout=90).stdout

lines = [l for l in out.splitlines() if l.startswith("WAPI ")]
if "WAPI done" not in out:
    print(out[-2000:])
    sys.exit("fixture did not run to completion")

fails = [l for l in lines if " ok" not in l and "done" not in l]
checks = [l for l in lines if l.strip() != "WAPI done"]
print(f"{len(checks)} documented widget methods exercised")
for f in fails:
    print("FAIL:", f)
if fails:
    sys.exit(1)
print("widget API: ok")
