#!/usr/bin/env python3
"""Regression test for the timer-accuracy bug class.

Six apps shipped with the same defect at once: a periodic timer re-arms AFTER
its callback returns, so timer.every always runs slow and the error
accumulates in one direction. Code that counts ticks to measure or pace time
is therefore wrong, always, and it is invisible on screen.

This runs sim/fixtures/timing.lua, which exercises both the wrong pattern and
the right one for the same nominal duration, and asserts:

  * the absolute-schedule pattern stays within tolerance, and
  * the tick-counting pattern is measurably worse

The second assertion matters as much as the first: without it, a change that
made every timer accurate (or the harness stop measuring) would leave the
first check passing for the wrong reason.

    sim/timing_test.py          # exits non-zero on failure
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SIM = os.path.join(HERE, "build", "sim")
FIXTURE = os.path.join("sim", "fixtures", "timing.lua")

# The fixture paces 40 x 100 ms = 4000 ms. Measured on a host: the absolute
# schedule lands within a millisecond or two, tick-counting is ~136 ms slow.
# 40 ms sits far from both, so this is neither flaky nor toothless.
TOLERANCE_MS = 40

if not os.path.exists(SIM):
    sys.exit("sim not built -- run sim/build.sh")

out = subprocess.run([SIM, "--sdroot", REPO, "run", FIXTURE, ":", "sleep", "7"],
                     cwd=REPO, capture_output=True, text=True, timeout=60).stdout

absolute = re.search(r"ABSOLUTE expected=(\d+) actual=(\d+) error=(-?\d+)", out)
tick = re.search(r"TICKCOUNT believed=(\d+) actual=(\d+) error=(-?\d+)", out)

if not absolute or not tick:
    print(out)
    sys.exit("timing fixture did not report; see output above")

abs_err = int(absolute.group(3))
tick_err = int(tick.group(3))
fails = []

if abs(abs_err) > TOLERANCE_MS:
    fails.append(f"absolute schedule drifted {abs_err:+d} ms over 4000 "
                 f"(tolerance +/-{TOLERANCE_MS}) -- timer.after chaining regressed")

if tick_err <= abs(abs_err):
    fails.append(f"tick-counting error ({tick_err:+d} ms) is no worse than the "
                 f"absolute schedule ({abs_err:+d} ms) -- this test has stopped "
                 f"measuring anything")

print(f"absolute schedule: {abs_err:+d} ms over 4000")
print(f"tick counting:     {tick_err:+d} ms over 4000 (expected: clearly worse)")

if fails:
    for f in fails:
        print("FAIL:", f)
    sys.exit(1)
print("timing: ok")
