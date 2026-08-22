#!/usr/bin/env python3
"""Chaos soak: cycle through apps, poke them at random, watch memory.

tools/soak.py holds ONE app up for a long time. This does the opposite --
it exercises the launch/exit path over and over with random interaction in
between, which is where a different class of bug lives: widget records that
outlive their app, event subscriptions that are not released, timer slots
that leak across launches, or a wedge that only appears after the Nth cycle.

The launch/exit cycle was measured leak-free once, but a great deal has
changed since, and every app now goes through more of the ui module.

The signal is the LAUNCHER-IDLE memory reading, sampled after STOP -- not
the in-app figure. In-app memory legitimately varies by app; what must come
back to the same number every time is free PSRAM with nothing running.

    tools/chaos.py 120                  # minutes
    tools/chaos.py 120 out.csv

Deliberately excludes the runaway/watchdog fixtures: they reboot the board
on purpose, which is a tested behaviour, not something to soak.
"""
import sys, glob, time, random, serial, datetime

# Fixtures that intentionally hang or reboot the board. Soaking them just
# measures the watchdog, and each reboot risks the native-USB wedge that
# needs a physical button dance to clear.
EXCLUDE = {
    "runaway_bare.lua", "runaway_coro.lua", "runaway_pcall.lua",
    "hook_bypass.lua", "headless.lua",
}

minutes = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
out = sys.argv[2] if len(sys.argv) > 2 else "chaos.csv"

# Fixed seed: a crash found here must be reproducible.
rng = random.Random(20260822)


def open_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        return None
    try:
        return serial.Serial(ports[0], 115200, timeout=2)
    except serial.SerialException:
        return None


def reconnect(timeout=120):
    deadline = time.time() + timeout
    while time.time() < deadline:
        s = open_port()
        if s is not None:
            return s
        time.sleep(5)
    return None


def cmd(s, line, wait=0.6):
    s.reset_input_buffer()
    s.write((line + "\n").encode())
    time.sleep(wait)
    return s.read(8000).decode(errors="replace")


s = reconnect()
if s is None:
    sys.exit("no board found")

listing = cmd(s, "LIST", 2.0)
apps = [l.split(None, 1)[1].strip()
        for l in listing.splitlines() if l.startswith("APP ")]
apps = [a for a in apps if a not in EXCLUDE]
if not apps:
    sys.exit(f"no apps to soak; LIST said:\n{listing}")
print(f"{len(apps)} apps in rotation", flush=True)

end = time.time() + minutes * 60
start = time.time()
cycles = errors = gaps = 0
baseline = None

with open(out, "a") as f:
    f.write(f"# chaos started {datetime.datetime.now().isoformat()} "
            f"over {len(apps)} apps\n")
    f.write("elapsed_s,cycle,app,idle_psram,idle_internal,largest,note\n")
    f.flush()

    while time.time() < end:
        app = rng.choice(apps)
        note = ""
        try:
            r = cmd(s, f"RUN {app}", 2.5)
            if "RUN_ERR" in r:
                note = "run_err"

            # Poke it. Taps land anywhere on the panel; swipes exercise
            # tileview paging and gesture handling.
            for _ in range(rng.randint(2, 6)):
                if rng.random() < 0.75:
                    cmd(s, f"TAP {rng.randint(4, 364)} {rng.randint(4, 444)}", 0.35)
                else:
                    cmd(s, f"SWIPE {rng.randint(40, 330)} {rng.randint(60, 400)} "
                           f"{rng.randint(40, 330)} {rng.randint(60, 400)} 220", 0.6)

            cmd(s, "STOP", 1.2)
            time.sleep(0.6)                       # let the launcher settle
            mem = cmd(s, "MEM", 0.8)
        except (serial.SerialException, OSError):
            mem = ""

        line = next((l for l in mem.splitlines() if l.startswith("MEM ")), None)
        if not line:
            gaps += 1
            print(f"link lost after {app}, reconnecting", flush=True)
            f.write(f"{int(time.time()-start)},{cycles},{app},,,,link_lost\n")
            f.flush()
            try:
                s.close()
            except Exception:
                pass
            s = reconnect()
            if s is None:
                f.write("# device never returned; chaos aborted\n")
                print("device never returned; aborted", flush=True)
                break
            continue

        p = line.split()
        cycles += 1
        idle = int(p[1])
        if baseline is None:
            baseline = idle
        # A single cycle drifting is noise; the trend is what matters, so
        # just record it and let the analysis decide.
        if note:
            errors += 1
        f.write(f"{int(time.time()-start)},{cycles},{app},{p[1]},{p[2]},{p[3]},{note}\n")
        f.flush()
        if cycles % 10 == 1:
            print(f"cycle {cycles}: {app} idle_psram={idle:,} "
                  f"(baseline {baseline:,}, delta {idle-baseline:+,})", flush=True)

print(f"chaos complete: {cycles} cycles, {errors} run errors, "
      f"{gaps} link drops -> {out}", flush=True)
