#!/usr/bin/env python3
"""Churn the shell: launch, exit, toggle and scan on a loop, and catch anomalies.

Different from soak.py, which runs ONE app for a long time and watches memory
drift. This exercises the TRANSITIONS -- launch, exit, BOOT toggle, the Wi-Fi
page -- because that is where this shell's bugs have actually lived: a screen
leak on the BOOT toggle, a sheet pointer left dangling, an app exiting on its
own. None of those show up in a sustained run.

    tools/churn.py 30            # minutes
    tools/churn.py 30 churn.csv

Every cycle checks that the app it launched is still resident, and the moment
one is not it dumps LOG. That is the point of the whole thing: an app exiting
unexpectedly was observed once and never reproduced, and the error text was
unreadable at the time. Now it gets captured automatically.

PSRAM is the residency test. An app VM plus its screen costs ~150 KB, which is
far outside sampling noise (measured drift with no app is single-digit bytes).
"""
import sys, glob, time, datetime, serial

minutes = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
out_path = sys.argv[2] if len(sys.argv) > 2 else "churn.csv"

# An app resident sits ~150 KB below the idle figure; anything above this is
# "no app running". Measured on the board, not guessed.
NO_APP_PSRAM = 5_000_000


def open_port(timeout=2):
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        return None
    try:
        return serial.Serial(ports[0], 115200, timeout=timeout)
    except (OSError, serial.SerialException):
        return None


def reconnect(timeout=90):
    """The board's native USB re-enumerates on its own during a long run, and
    comes back under a different name. Wait it out rather than dying."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        s = open_port()
        if s:
            return s
        time.sleep(2)
    return None


def cmd(s, line, expect, wait=6.0):
    """Send one command and return its reply.

    Returns the reply line whether or not it matched `expect` -- an ERR reply
    is an ANSWER, and the first version of this treated "RUN_ERR
    already_running" as "the board did not respond". That mislabelled a real
    bug (an app stuck running, every later RUN refused) as a dead link for 140
    cycles, and buried it in the noise. Distinguish: None means genuinely no
    reply; a string starting with something other than `expect` is a refusal.
    """
    s.reset_input_buffer()
    s.write((line + "\n").encode())
    s.flush()
    deadline = time.time() + wait
    verb = line.split()[0]
    while time.time() < deadline:
        try:
            got = s.readline().decode("utf-8", "replace").strip()
        except (OSError, serial.SerialException):
            return None
        if got.startswith(expect):
            return got
        # The matching error reply for this verb, e.g. RUN_ERR for RUN.
        if got.startswith(verb + "_ERR"):
            return got
    return None


def psram(s):
    """Free PSRAM via STATS, or None if it did not answer."""
    s.reset_input_buffer()
    s.write(b"STATS\n")
    s.flush()
    deadline = time.time() + 6
    while time.time() < deadline:
        try:
            ln = s.readline().decode("utf-8", "replace")
        except (OSError, serial.SerialException):
            return None
        if "psram" in ln and "free" in ln:
            for tok in ln.replace(",", "").split():
                if tok.isdigit():
                    return int(tok)
        if ln.startswith("STATS_END"):
            return None
    return None


def dump_log(s):
    s.reset_input_buffer()
    s.write(b"LOG\n")
    s.flush()
    lines, deadline = [], time.time() + 10
    while time.time() < deadline:
        try:
            ln = s.readline().decode("utf-8", "replace").rstrip()
        except (OSError, serial.SerialException):
            break
        if ln.startswith("LOG_END"):
            break
        lines.append(ln)
    return lines


APPS = ["counter.lua", "stopwatch.lua", "dice.lua", "tally.lua"]

s = open_port()
if s is None:
    sys.exit("no /dev/cu.usbmodem* -- is the board attached?")

end = time.time() + minutes * 60
cycle = anomalies = 0
log = open(out_path, "a", buffering=1)
log.write("# ts,cycle,phase,psram,note\n")
print(f"churn: {minutes:g} min, logging to {out_path}")

while time.time() < end:
    cycle += 1
    app = APPS[cycle % len(APPS)]
    ts = datetime.datetime.now().isoformat(timespec="seconds")

    try:
        # 1. Launch, and confirm it is actually resident.
        r = cmd(s, f"RUN {app}", "RUN_OK")
        if r is None:
            log.write(f"{ts},{cycle},run,,no reply to RUN {app} -- link?\n")
            s = reconnect() or s
            continue
        if not r.startswith("RUN_OK"):
            # A refusal, not silence. "already_running" means a previous app
            # never exited -- the interesting failure, and worth the log dump
            # right now while the cause is still in the ring.
            anomalies += 1
            print(f"cycle {cycle}: RUN refused -- {r}")
            log.write(f"{ts},{cycle},run,,ANOMALY {r}\n")
            for ln in dump_log(s):
                log.write(f"# LOG {ln}\n")
            # Clear it so the run can continue: BOOT twice, because the first
            # press is eaten as a wake if the screen has slept (see below).
            cmd(s, "BOOT", "BOOT_OK")
            time.sleep(1.0)
            cmd(s, "BOOT", "BOOT_OK")
            time.sleep(1.0)
            continue
        time.sleep(2.5)
        p = psram(s)
        if p is not None and p > NO_APP_PSRAM:
            anomalies += 1
            print(f"cycle {cycle}: {app} EXITED on its own (psram {p:,})")
            log.write(f"{ts},{cycle},resident,{p},ANOMALY {app} not resident\n")
            for ln in dump_log(s):
                log.write(f"# LOG {ln}\n")
        else:
            log.write(f"{ts},{cycle},resident,{p if p else ''},{app}\n")

        # 2. BOOT out of it, then toggle the shell both ways.
        #
        # A serial-driven run has no touch activity, so after SCREEN_SLEEP_MS
        # the panel sleeps and the FIRST BOOT is consumed waking it -- by
        # design, so a press on a dark screen cannot stop an app you cannot
        # see. TAP first to reset LVGL's inactivity clock, which keeps the
        # screen awake and makes each BOOT below mean what it says.
        cmd(s, "TAP 4 4", "TAP_OK")   # a corner: wakes without hitting a control
        time.sleep(0.4)
        for label in ("exit app", "face -> apps", "apps -> face"):
            if cmd(s, "BOOT", "BOOT_OK") is None:
                log.write(f"{ts},{cycle},boot,,no reply to BOOT ({label})\n")
            time.sleep(1.1)

        p = psram(s)
        log.write(f"{ts},{cycle},idle,{p if p else ''},after toggles\n")

        # 3. Every 5th cycle, the Wi-Fi page -- the surface the one observed
        #    unexplained exit came from.
        if cycle % 5 == 0:
            cmd(s, "RUN settings.lua", "RUN_OK")
            time.sleep(2.0)
            cmd(s, "TAP 184 268", "TAP_OK")
            time.sleep(5.0)
            p = psram(s)
            if p is not None and p > NO_APP_PSRAM:
                anomalies += 1
                print(f"cycle {cycle}: settings EXITED on the Wi-Fi page (psram {p:,})")
                log.write(f"{ts},{cycle},wifi,{p},ANOMALY settings not resident\n")
                for ln in dump_log(s):
                    log.write(f"# LOG {ln}\n")
            else:
                log.write(f"{ts},{cycle},wifi,{p if p else ''},settings ok\n")
            cmd(s, "STOP", "STOP_OK")
            time.sleep(1.0)

    except (OSError, serial.SerialException):
        log.write(f"{ts},{cycle},,,serial dropped -- reconnecting\n")
        try:
            s.close()
        except Exception:
            pass
        ns = reconnect()
        if ns is None:
            log.write(f"{ts},{cycle},,,reconnect FAILED -- board wedged?\n")
            print("board did not come back -- stopping")
            break
        s = ns

print(f"churn: {cycle} cycles, {anomalies} anomalies -> {out_path}")
log.close()
