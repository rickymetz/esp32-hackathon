#!/usr/bin/env python3
"""Drive the device UI over serial: a command chain for agents.

Usage: drive.py CMD [args] [: CMD [args]]...

  run <app.lua>            launch an app (STOPs anything running first)
  stop                     stop the running app
  tap <x> <y>              synthetic tap
  swipe <x0> <y0> <x1> <y1> [ms]   synthetic swipe/drag
  sleep <seconds>          wait (UI settle, animations)
  shot <out.png>           capture the screen

Example -- open the keyboard and look at it:
  drive.py run ui_test.lua : sleep 1 : tap 184 390 : sleep 0.6 : shot kb.png
"""
import sys, glob, time, subprocess, serial

args = sys.argv[1:]
if not args:
    sys.exit(__doc__)

ports = sorted(glob.glob("/dev/cu.usbmodem*"))
if not ports:
    sys.exit("no board found")
PORT = ports[0]

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

s = serial.Serial(PORT, 115200, timeout=2)
for c in chains(args):
    op = c[0]
    if op == "run":
        cmd_and_wait(s, "STOP", "STOP_OK", "STOP_ERR", 4)
        time.sleep(1.0)
        print("run:", cmd_and_wait(s, f"RUN {c[1]}", "RUN_OK", "RUN_ERR"))
    elif op == "stop":
        print("stop:", cmd_and_wait(s, "STOP", "STOP_OK", "STOP_ERR", 4))
    elif op == "pwr":
        print("pwr:", cmd_and_wait(s, "PWR", "PWR_OK", None))
    elif op == "tap":
        print("tap:", cmd_and_wait(s, f"TAP {c[1]} {c[2]}", "TAP_OK", "TAP_ERR"))
    elif op == "swipe":
        print("swipe:", cmd_and_wait(s, "SWIPE " + " ".join(c[1:]), "SWIPE_OK", "SWIPE_ERR"))
    elif op == "sleep":
        time.sleep(float(c[1]))
    elif op == "shot":
        s.close()
        subprocess.run([sys.executable, __file__.replace("drive.py", "screenshot.py"), c[1], PORT], check=False)
        s = serial.Serial(PORT, 115200, timeout=2)
    else:
        sys.exit(f"unknown command: {op}")
s.close()
