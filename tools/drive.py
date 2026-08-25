#!/usr/bin/env python3
"""Drive the device UI over serial: a command chain for agents.

Usage: drive.py CMD [args] [: CMD [args]]...

  push <path>              install a .lua file or folder app over USB
  run <app.lua>            launch an app (STOPs anything running first)
  stop                     stop the running app
  pwr [down|up|long]       inject the PWR button (a quick click by default)
  tap <x> <y>              synthetic tap
  swipe <x0> <y0> <x1> <y1> [ms]   synthetic swipe/drag
  sleep <seconds>          wait (UI settle, animations)
  shot <out.png>           capture the screen
  stats                    heap low-water, per-task CPU and stack headroom

Example -- open the keyboard and look at it:
  drive.py run ui_test.lua : sleep 1 : tap 184 390 : sleep 0.6 : shot kb.png
  (fixtures live in tests/fixtures/; push.py installs them by basename)

The whole edit/install/see loop in one command:
  drive.py push apps/myapp.lua : run myapp.lua : sleep 1 : shot out.png

EXIT CODE: non-zero if any step in the chain failed. This matters more than it
looks -- the point of this tool is unattended verification, so a chain that
prints RUN_ERR and exits 0 is worse than useless: every caller that checks the
exit code (CI, a script, an agent) is told the run succeeded.
"""
import sys, glob, time, subprocess, serial

args = sys.argv[1:]
if not args:
    sys.exit(__doc__)


def find_port():
    """Pick the board's port, and confirm something answering the launcher
    protocol is actually on it.

    Every tool here globs /dev/cu.usbmodem* and takes the first hit. That is a
    guess: this board's native USB re-enumerates on its own and comes back
    under a different name (see CLAUDE.md), and any other usbmodem device --
    another dev board, a phone -- sorts into the same list. PING is what turns
    the guess into a check.
    """
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("no board found")
    if len(ports) == 1:
        return ports[0]           # nothing to disambiguate; skip the probe

    for p in ports:
        try:
            with serial.Serial(p, 115200, timeout=1) as probe:
                probe.reset_input_buffer()
                probe.write(b"PING\n")
                deadline = time.time() + 1.5
                while time.time() < deadline:
                    if probe.readline().decode("utf-8", "replace").startswith("PONG launcher"):
                        return p
        except serial.SerialException:
            continue              # busy or not openable -- try the next one
    print(f"warning: no port answered PING; falling back to {ports[0]}", file=sys.stderr)
    return ports[0]


PORT = find_port()
FAILED = False        # sticky: any failing step makes the whole chain fail


def fail(msg):
    global FAILED
    FAILED = True
    print(msg, file=sys.stderr)


def chains(argv):
    cur = []
    for a in argv:
        if a == ":":
            if cur: yield cur
            cur = []
        else:
            cur.append(a)
    if cur: yield cur


def cmd_and_wait(s, line, ok, err, timeout=8):
    s.write((line + "\n").encode())
    deadline = time.time() + timeout
    while time.time() < deadline:
        got = s.readline().decode("utf-8", "replace").strip()
        if got.startswith(ok):
            return got
        if err and got.startswith(err):
            return got
    return "timeout"


def need(c, n, usage):
    """Guard a verb's arguments so a missing one is a usage message, not a
    traceback from indexing c[1]."""
    if len(c) <= n:
        sys.exit(f"usage: {usage}")


def sub(script, *argv):
    """Run a sibling tool with the port to itself, and PROPAGATE its exit
    code -- shot/stats used to discard it with check=False."""
    r = subprocess.run([sys.executable, __file__.replace("drive.py", script), *argv],
                       check=False)
    if r.returncode != 0:
        fail(f"{script} failed (exit {r.returncode})")
    return r.returncode


s = serial.Serial(PORT, 115200, timeout=2)
for c in chains(args):
    op = c[0]
    if op == "push":
        need(c, 1, "push <path>")
        # push.py owns the PUSH protocol (CRC, folder apps, icon building), so
        # shell out rather than duplicating it. It needs the port to itself.
        s.close()
        rc = sub("push.py", c[1], PORT)
        s = serial.Serial(PORT, 115200, timeout=2)
        if rc != 0:
            break            # nothing downstream can be meaningful
    elif op == "run":
        need(c, 1, "run <app.lua>")
        stopped = cmd_and_wait(s, "STOP", "STOP_OK", "STOP_ERR", 4)
        # Skip the settle sleep ONLY when the device confirmed nothing was
        # running. A "timeout" is not that: it usually means the stop DID
        # happen and the reply was lost, and skipping the wait then races the
        # teardown so the RUN below comes back already_running.
        if not stopped.startswith("STOP_ERR"):
            time.sleep(1.0)
        got = cmd_and_wait(s, f"RUN {c[1]}", "RUN_OK", "RUN_ERR")
        print("run:", got)
        if not got.startswith("RUN_OK"):
            fail(f"run {c[1]}: {got}")
    elif op == "stop":
        got = cmd_and_wait(s, "STOP", "STOP_OK", "STOP_ERR", 4)
        print("stop:", got)
        # STOP_ERR (nothing running) is a fine outcome for a `stop` verb; a
        # timeout is not -- the device did not answer at all.
        if got == "timeout":
            fail("stop: timeout")
    elif op == "pwr":
        got = cmd_and_wait(s, "PWR", "PWR_OK", None)
        print("pwr:", got)
        if not got.startswith("PWR_OK"):
            fail(f"pwr: {got}")
    elif op == "tap":
        need(c, 2, "tap <x> <y>")
        got = cmd_and_wait(s, f"TAP {c[1]} {c[2]}", "TAP_OK", "TAP_ERR")
        print("tap:", got)
        if not got.startswith("TAP_OK"):
            fail(f"tap: {got}")
    elif op == "swipe":
        need(c, 4, "swipe <x0> <y0> <x1> <y1> [ms]")
        got = cmd_and_wait(s, "SWIPE " + " ".join(c[1:]), "SWIPE_OK", "SWIPE_ERR")
        print("swipe:", got)
        if not got.startswith("SWIPE_OK"):
            fail(f"swipe: {got}")
    elif op == "sleep":
        need(c, 1, "sleep <seconds>")
        time.sleep(float(c[1]))
    elif op == "stats":
        # CPU is a delta between consecutive STATS calls, so a chain wants
        # this twice around the thing being measured:
        #   drive.py run x.lua : stats : sleep 5 : stats
        # The first call primes; the second reports the interval.
        s.close()
        sub("stats.py")
        s = serial.Serial(PORT, 115200, timeout=2)
    elif op == "shot":
        need(c, 1, "shot <out.png>")
        s.close()
        sub("screenshot.py", c[1], PORT)
        s = serial.Serial(PORT, 115200, timeout=2)
    else:
        sys.exit(f"unknown command: {op}")
s.close()

if FAILED:
    sys.exit(1)
