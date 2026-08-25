#!/usr/bin/env python3
"""Regression test for the periodic-timer catch-up guard.

`app_timer_run_due()` advances a periodic deadline by its period so ticks sit
on a fixed grid. That raises a question the plain `next += period` does not
answer: what if the callback takes LONGER than the period? The deadline is then
already in the past when it returns, and firing once per missed slot to "catch
up" would run callbacks back-to-back and starve the app.

The guard skips the missed slots instead, snapping forward to the next slot on
the original grid. Nothing exercised that branch -- it was the one piece of the
timer work with no coverage at all -- so this does.

`sim/fixtures/overrun.lua` runs a `timer.every(50, ...)` whose callback busy-
waits 120 ms. Asserted:

  * ticks are SKIPPED, not replayed: 12 fires of a 50 ms timer whose body costs
    120 ms must take roughly 12 x 150 ms (the next 50 ms slot at or past
    120 ms), never 12 x 50 ms;
  * no burst: the smallest gap between consecutive fires is at least the
    interval, which is what a replayed backlog would violate;
  * phase is preserved: gaps are whole multiples of the interval.

    sim/overrun_test.py          # exits non-zero on failure
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SIM = os.path.join(HERE, "build", "sim")
FIXTURE = os.path.join("sim", "fixtures", "overrun.lua")

if not os.path.exists(SIM):
    sys.exit("sim not built -- run sim/build.sh")

out = subprocess.run([SIM, "--sdroot", REPO, "run", FIXTURE, ":", "sleep", "6"],
                     cwd=REPO, capture_output=True, text=True, timeout=90).stdout

m = re.search(r"OVERRUN fires=(\d+) elapsed=(\d+) interval=(\d+) busy=(\d+) min_gap=(-?\d+)", out)
if not m:
    print(out)
    sys.exit("overrun fixture did not report; see output above")

fires, elapsed, interval, busy, min_gap = (int(g) for g in m.groups())

# The first slot at or past `busy` on an `interval` grid.
expected_gap = ((busy + interval - 1) // interval) * interval      # 120,50 -> 150

# Tolerances are tight ON PURPOSE, and were calibrated by deleting the guard and
# re-running rather than guessed. Without it the timer fires the instant each
# callback returns, so gaps collapse to the callback's own duration (~126 ms
# measured) instead of the next grid slot (~148 ms measured). A loose bound
# passes both and tests nothing -- the first version of this file did exactly
# that, and only caught it because the no-guard build was run against it.
GAP_TOL = interval // 3            # 16 ms: admits 148, rejects 126
ELAPSED_FLOOR = expected_gap * fires * 0.85
fails = []

if elapsed < ELAPSED_FLOOR:
    fails.append(
        f"{fires} fires took {elapsed} ms, under the {ELAPSED_FLOOR:.0f} ms floor; a "
        f"{interval} ms timer with a {busy} ms body cannot beat ~{expected_gap} ms/fire, "
        f"so ticks were replayed back-to-back instead of skipped")

if min_gap < interval:
    fails.append(
        f"smallest gap between fires was {min_gap} ms, below the {interval} ms interval "
        f"-- that is a catch-up burst, which the guard exists to prevent")

if abs(min_gap - expected_gap) > GAP_TOL:
    fails.append(
        f"smallest gap {min_gap} ms is not within {GAP_TOL} ms of the next grid slot "
        f"({expected_gap} ms) -- the guard is missing, or it re-based the phase on now "
        f"instead of snapping forward on the original grid")

print(f"fires={fires} elapsed={elapsed} ms  min_gap={min_gap} ms "
      f"(expected ~{expected_gap} ms/fire)")

if fails:
    for f in fails:
        print("FAIL:", f)
    sys.exit(1)
print("overrun: ok")
