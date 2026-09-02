#!/usr/bin/env python3
"""Documented behaviour that had never actually worked.

Every case here is a promise APP_CONTRACT.md makes to the five people writing
apps, and every one failed SILENTLY -- a no-op setter, a getter returning nil,
an image that draws nothing -- which is exactly why none were noticed until an
adversarial review went looking.

Confirmed to fail without the fixes rather than assumed to: card-relative
image paths measured 0 px against 128 px for the "D:" spelling of the same
file, and both ui.row directions returned the wrong thing.

    sim/promises_test.py
"""
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SIM = os.path.join(HERE, "build", "sim")
FIXTURE = os.path.join("sim", "fixtures", "promises.lua")

if not os.path.exists(SIM):
    sys.exit("sim not built -- run sim/build.sh")

out = subprocess.run([SIM, "--sdroot", REPO, "run", FIXTURE, ":", "sleep", "2.5"],
                     cwd=REPO, capture_output=True, text=True, timeout=90).stdout

lines = [l for l in out.splitlines() if l.startswith("PROM ")]
if "PROM done" not in out:
    print(out[-2000:])
    sys.exit("fixture did not run to completion")

fails = [l for l in lines if " ok" not in l and "done" not in l]
checks = [l for l in lines if l.strip() != "PROM done"]
print(f"{len(checks)} documented promises exercised")
for f in fails:
    print("FAIL:", f)
if fails:
    sys.exit(1)
print("promises: ok")
