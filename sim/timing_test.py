#!/usr/bin/env python3
"""Regression test for timer accuracy.

History, because it explains the shape of this test. Six apps shipped with the
same defect at once: app_timer.c re-armed a periodic timer from the moment its
callback RETURNED (`next_us = now + period`), so timer.every always ran slow
and the error accumulated in one direction forever. This test used to assert
that tick-counting was *measurably worse* than an absolute schedule -- a
tripwire so the test could not quietly stop measuring anything.

That tripwire fired, as intended, when app_timer.c was fixed to advance from
the previous deadline (`next_us += period`) instead. Measured on hardware
before/after: 5.0 ms/tick -> 0.0 ms/tick on a 1000 ms timer.

So the assertion is now inverted: BOTH patterns must stay within tolerance.
That keeps the teeth pointed at the thing that can actually regress -- if
anyone restores the re-arm-from-now behaviour, tick-counting drifts linearly
and blows the tolerance within this fixture's 4 s run.

  * the absolute-schedule pattern stays within tolerance (timer.after chaining)
  * the tick-counting pattern stays within tolerance (timer.every's grid)

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

# The fixture paces 40 x 100 ms = 4000 ms. With the re-arm bug present,
# tick-counting measured ~136 ms slow over that run; with it fixed both
# patterns land within a millisecond or two on a host. 40 ms sits far from
# both, so this is neither flaky nor toothless: a reverted app_timer.c fails
# it by a wide margin.
TOLERANCE_MS = 40

if not os.path.exists(SIM):
    sys.exit("sim not built -- run sim/build.sh")

out = subprocess.run([SIM, "--sdroot", REPO, "run", FIXTURE, ":", "sleep", "7"],
                     cwd=REPO, capture_output=True, text=True, timeout=60).stdout

absolute = re.search(r"ABSOLUTE expected=(\d+) actual=(\d+) error=(-?\d+)", out)
tick = re.search(r"TICKCOUNT believed=(\d+) actual=(\d+) error=(-?\d+)", out)

# Re-armed tripwire. The original test asserted tick-counting stayed WORSE than
# the absolute schedule, specifically so it could not pass while measuring
# nothing; fixing app_timer.c made both accurate and retired that assertion.
# Two "within tolerance" bounds alone are satisfied vacuously by a fixture that
# reports 0 for the wrong reason (a broken fixture, a parse that matched the
# wrong line). So also assert the fixture actually did the work it claims:
# BEATS x INTERVAL of wall-clock, and a believed figure that matches it.
EXPECT_MS = 4000

if not absolute or not tick:
    print(out)
    sys.exit("timing fixture did not report; see output above")

abs_err = int(absolute.group(3))
tick_err = int(tick.group(3))
fails = []

if abs(abs_err) > TOLERANCE_MS:
    fails.append(f"absolute schedule drifted {abs_err:+d} ms over 4000 "
                 f"(tolerance +/-{TOLERANCE_MS}) -- timer.after chaining regressed")

if int(absolute.group(1)) != EXPECT_MS or int(tick.group(1)) != EXPECT_MS:
    fails.append(f"fixture did not pace {EXPECT_MS} ms "
                 f"(absolute expected={absolute.group(1)}, tick believed={tick.group(1)}) "
                 f"-- the measurement itself is broken, so the bounds below prove nothing")

if abs(tick_err) > TOLERANCE_MS:
    fails.append(f"timer.every drifted {tick_err:+d} ms over 4000 "
                 f"(tolerance +/-{TOLERANCE_MS}) -- app_timer.c is re-arming "
                 f"from dispatch time again instead of advancing the deadline")

print(f"absolute schedule: {abs_err:+d} ms over 4000")
print(f"tick counting:     {tick_err:+d} ms over 4000")

if fails:
    for f in fails:
        print("FAIL:", f)
    sys.exit(1)
print("timing: ok")
