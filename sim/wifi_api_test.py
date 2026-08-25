#!/usr/bin/env python3
"""Contract test for the wifi scan API, against the sim stub.

app_wifi.c is not compiled into the sim, so this cannot test the device.
What it DOES pin down is the shape every caller depends on: the nil-while-
scanning branch, idempotent reads, field names and types, and RSSI ordering.
Device behaviour is verified on hardware.

sim/wifi_parity_test.py guards the other half -- that the device exposes the
same function names this pins the semantics of.

    sim/wifi_api_test.py        # exits non-zero on failure
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SIM = os.path.join(HERE, "build", "sim")
FIXTURE = os.path.join("sim", "fixtures", "wifi_api.lua")

if not os.path.exists(SIM):
    sys.exit("sim not built -- run sim/build.sh")

out = subprocess.run([SIM, "--sdroot", REPO, "run", FIXTURE, ":", "sleep", "3"],
                     cwd=REPO, capture_output=True, text=True, timeout=60).stdout

fails = []


def need(pattern, why):
    m = re.search(pattern, out)
    if not m:
        fails.append(f"{why} (no match for {pattern!r})")
    return m


need(r"PRESCAN nil", "scan_results() must be nil before any scan")
need(r"START ok=true err=nil", "scan_start() must succeed on an idle stub")
need(r"RESTART true", "scan_start() during a scan must return true, not error")
need(r"SAWNIL true", "the still-scanning branch was never reached -- the stub "
                     "resolved instantly, so the nil path is untested")
need(r"DONE", "fixture never completed")

m = need(r"COUNT (\d+)", "no result count")
if m and int(m.group(1)) < 2:
    fails.append(f"expected >=2 fake networks, got {m.group(1)}")

again = re.search(r"AGAIN (\d+)", out)
if m and again and m.group(1) != again.group(1):
    fails.append(f"scan_results() is not idempotent: {m.group(1)} then {again.group(1)}")

nets = re.findall(r"NET \d+ ssid=(\S+) rssi=(-?\d+) secure=(true|false)", out)
if not nets:
    fails.append("no NET lines -- field names or types are wrong")
else:
    rssis = [int(r) for _, r, _ in nets]
    if rssis != sorted(rssis, reverse=True):
        fails.append(f"results not sorted strongest-first: {rssis}")
    if not any(s == "true" for _, _, s in nets):
        fails.append("no secured network in the fake list -- the lock-glyph "
                     "path in wifi_setup.lua would go untested")
    if not any(s == "false" for _, _, s in nets):
        fails.append("no open network in the fake list -- the connect-without-"
                     "password path would go untested")

if fails:
    print(out.strip()[-400:])
    for f in fails:
        print("FAIL:", f)
    sys.exit(1)
print(f"wifi api: {len(nets)} networks, ordering ok")
print("wifi api: ok")
