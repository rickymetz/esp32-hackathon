#!/usr/bin/env python3
"""Fuzz every app with random input, hunting crashes.

test.sh asks "does this app draw something". scenarios.py asserts a handful
of specific interactions. Neither goes DEEP: they never open a picker inside
a sheet, drag a slider to its stop, mash a confirm dialog, or swipe a
tileview while a modal is up. Those paths run through the Lua<->LVGL
bindings and the ui module -- shared code that all six developers' apps sit
on, where one bad object lifetime crashes everybody.

So: drive each app with long random sequences of taps and swipes and treat
ANY of these as a finding --

  * a Lua error ("app '...' failed:")
  * an event-callback error (tag lua_lvgl_evt)
  * an LVGL assert, an abort, or a non-zero exit
  * a hang (the run is killed at a timeout)

Every sequence is generated from a recorded seed, so anything it finds
replays exactly.

    sim/fuzz.py                 # default rounds
    sim/fuzz.py --rounds 40 --jobs 8
"""
import argparse, concurrent.futures, glob, os, random, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SIM = os.path.join(HERE, "build", "sim")

# Fixtures that fail or hang ON PURPOSE. Including them would drown the
# real findings in expected noise.
EXCLUDE = {
    "broken.lua", "deep_error.lua", "cb_error.lua",
    "runaway_bare.lua", "runaway_coro.lua", "runaway_pcall.lua",
    "hook_bypass.lua", "headless.lua", "trim_check.lua",
}

FAIL_PATTERNS = [
    (re.compile(r"app '.*?' failed:"), "lua error"),
    (re.compile(r"lua_lvgl_evt"), "error inside an event callback"),
    (re.compile(r"LV_ASSERT|lv_assert"), "LVGL assert"),
    (re.compile(r"Assertion .* failed|abort\(\)|Segmentation fault"), "abort/segfault"),
]


def sequence(rng, n):
    """A random interaction chain in drive.py's command grammar."""
    out = ["sleep", "0.4"]
    for _ in range(n):
        r = rng.random()
        if r < 0.6:
            out += [":", "tap", str(rng.randint(4, 364)), str(rng.randint(4, 444))]
        elif r < 0.85:
            out += [":", "swipe",
                    str(rng.randint(30, 340)), str(rng.randint(50, 410)),
                    str(rng.randint(30, 340)), str(rng.randint(50, 410)), "200"]
        else:
            out += [":", "sleep", "0.3"]
    return out


def run_one(app, seed, steps):
    rng = random.Random(seed)
    cmds = [SIM, "--sdroot", REPO, "run", f"apps/{app}"] + sequence(rng, steps)
    try:
        p = subprocess.run(cmds, cwd=REPO, capture_output=True,
                           text=True, timeout=90)
        out = p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        return app, seed, "HANG (killed at 90s)", ""
    if p.returncode != 0:
        return app, seed, f"exit {p.returncode}", out[-1500:]
    for pat, label in FAIL_PATTERNS:
        if pat.search(out):
            return app, seed, label, out[-1500:]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=12, help="seeds per app")
    ap.add_argument("--steps", type=int, default=14, help="interactions per run")
    ap.add_argument("--jobs", type=int, default=6)
    a = ap.parse_args()

    if not os.path.exists(SIM):
        sys.exit("sim not built -- run sim/build.sh")

    apps = sorted(os.path.basename(p) for p in glob.glob(os.path.join(REPO, "apps", "*.lua")))
    apps = [x for x in apps if x not in EXCLUDE]

    jobs = [(app, 1000 + i) for app in apps for i in range(a.rounds)]
    print(f"fuzzing {len(apps)} apps x {a.rounds} seeds x {a.steps} steps "
          f"= {len(jobs)} runs", flush=True)

    findings, done = [], 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        futs = {ex.submit(run_one, app, seed, a.steps): (app, seed) for app, seed in jobs}
        for fut in concurrent.futures.as_completed(futs):
            done += 1
            r = fut.result()
            if r:
                app, seed, why, tail = r
                findings.append(r)
                print(f"\nFINDING  {app}  seed={seed}  {why}", flush=True)
                if tail:
                    print("  " + "\n  ".join(tail.strip().splitlines()[-12:]), flush=True)
            if done % 25 == 0:
                print(f"  {done}/{len(jobs)} runs, {len(findings)} findings", flush=True)

    print(f"\nfuzz: {done} runs, {len(findings)} findings")
    if findings:
        print("\nreplay a finding with:")
        app, seed, _, _ = findings[0]
        print(f"  sim/fuzz.py --rounds 1 --steps {a.steps}   # seed {seed}, app {app}")
        for app, seed, why, _ in findings:
            print(f"  - {app} seed={seed}: {why}")
        sys.exit(1)
    print("no crashes found")


main()
