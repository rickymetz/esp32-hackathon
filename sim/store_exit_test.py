#!/usr/bin/env python3
"""An app that never calls store.save() must not lose its state on exit.

BOOT lands whenever the user presses it and there is no on_exit hook to save
from, so "call save() after every mutation" was a rule that could only be
followed perfectly or not at all -- and getting it wrong lost data in silence.
The launcher now writes a dirty store itself during teardown.

This drives three launches in ONE chain, because the simulator only tears an
app down when another starts or `stop` runs -- it exits the process without
teardown otherwise, which is a real sim/device difference worth knowing when
reading this file.

    sim/store_exit_test.py
"""
import json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SIM = os.path.join(HERE, "build", "sim")
FIXTURE = os.path.join("sim", "fixtures", "store_noSave.lua")
STATE = os.path.join(REPO, "state", "store_noSave.json")

if not os.path.exists(SIM):
    sys.exit("sim not built -- run sim/build.sh")

if os.path.exists(STATE):
    os.remove(STATE)

chain = []
for _ in range(3):
    chain += ["run", FIXTURE, ":", "sleep", "0.4", ":"]
out = subprocess.run([SIM, "--sdroot", REPO] + chain[:-1],
                     cwd=REPO, capture_output=True, text=True, timeout=120).stdout

seen = [int(l.split("=")[1]) for l in out.splitlines() if l.startswith("STORE read runs=")]
fails = []

if seen != [0, 1, 2]:
    fails.append(f"expected the count to survive each exit (0, 1, 2), got {seen}")

if not os.path.exists(STATE):
    fails.append("no state file was written at all")
else:
    with open(STATE) as f:
        got = json.load(f)
    if got.get("runs") != 3:
        fails.append(f'expected {{"runs": 3}} on disk, got {got}')

for f in fails:
    print("FAIL:", f)
if fails:
    sys.exit(1)
print("store exit flush: ok (unsaved state survived 3 exits)")
