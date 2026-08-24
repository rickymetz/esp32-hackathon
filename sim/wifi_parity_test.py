#!/usr/bin/env python3
"""The sim's wifi stub must expose exactly the device's wifi API.

sim/src/sim_wifi.c substitutes for app_wifi.c rather than wrapping it, so the
two can drift apart silently: a function added to one and not the other means
an app passes headlessly and breaks on hardware, or the reverse. Neither the
sim suite nor a device test catches that, because each only ever sees one side.

Compares the two wifi_funcs[] tables by name. It cannot check behaviour --
sim/wifi_api_test.py pins the semantics, and the device is verified on
hardware -- but a name mismatch is the failure that would otherwise reach a
PR unnoticed.

    sim/wifi_parity_test.py     # exits non-zero on mismatch
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEV = os.path.join(REPO, "launcher/components/lua_module_wifi/app_wifi.c")
SIM = os.path.join(HERE, "src/sim_wifi.c")


def strip_comments(src):
    """Remove C comments before parsing.

    Without this the table regex happily matches inside /* ... */, so
    commenting a function out -- the most likely way someone disables one --
    left this test passing. Found by trying exactly that.
    """
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def names(path):
    src = strip_comments(open(path).read())
    m = re.search(r"wifi_funcs\[\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        sys.exit(f"no wifi_funcs[] table found in {path}")
    found = set(re.findall(r'\{\s*"(\w+)"\s*,', m.group(1)))
    if not found:
        sys.exit(f"wifi_funcs[] in {path} parsed as empty -- the regex has "
                 f"gone stale, which would make this test vacuously pass")
    return found


dev, sim = names(DEV), names(SIM)
only_dev, only_sim = sorted(dev - sim), sorted(sim - dev)

if only_dev or only_sim:
    if only_dev:
        print(f"FAIL: on the device but not in the sim: {only_dev}")
        print("      apps using these break headlessly (nil value) -- add stubs.")
    if only_sim:
        print(f"FAIL: in the sim but not on the device: {only_sim}")
        print("      apps using these pass in CI and break on hardware.")
    sys.exit(1)

print(f"wifi parity: ok ({len(dev)} functions: {', '.join(sorted(dev))})")
