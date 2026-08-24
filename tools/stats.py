#!/usr/bin/env python3
"""Read the launcher's STATS command: heap low-water marks, per-task CPU,
per-task stack headroom.

    tools/stats.py                 # one snapshot
    tools/stats.py 5               # 5 snapshots, 1s apart
    tools/stats.py 60 0.5 out.csv  # 60 samples at 0.5s, also written as CSV

CPU is measured over the interval between consecutive STATS calls, so the
FIRST sample of a run is meaningless -- there is no previous call to
difference against, and everything reads as since-boot. This tool always
takes a throwaway priming sample for that reason; what it prints is
always an interval measurement.

Both cores contribute to the run-time total, so the CPU column sums to
roughly 2000 permille (200%) across a snapshot, idle tasks included.
"""
import sys, glob, time, serial

INTERVAL_DEFAULT = 1.0


def open_port():
    ports = sorted(glob.glob('/dev/cu.usbmodem*'))
    if not ports:
        sys.exit("no board found on /dev/cu.usbmodem*")
    return serial.Serial(ports[0], 115200, timeout=2)


def sample(s):
    """Send STATS and parse the reply into a dict. Returns None on timeout."""
    s.reset_input_buffer()
    s.write(b"STATS\n")
    s.flush()

    out = {"heap": {}, "tasks": [], "uptime_ms": None}
    deadline = time.time() + 5
    started = False
    while time.time() < deadline:
        line = s.readline().decode(errors="replace").strip()
        if not line:
            continue
        if line == "STATS_BEGIN":
            started = True
            continue
        if line == "STATS_END":
            return out if started else None
        if not started or not line.startswith("STAT "):
            continue

        f = line.split()
        if f[1] == "uptime_ms":
            out["uptime_ms"] = int(f[2])
        elif f[1] in ("psram", "internal"):
            # STAT psram free <n> min <n> [largest <n>]
            pairs = f[2:]
            out["heap"][f[1]] = {pairs[i]: int(pairs[i + 1])
                                 for i in range(0, len(pairs) - 1, 2)}
        elif f[1] == "task":
            # Locate the keys rather than indexing by position: FreeRTOS
            # task names can contain spaces ("Tmr Svc" is a stock one), so
            # fixed offsets shift on exactly those rows.
            try:
                ci = f.index("cpu_permille")
                si = f.index("stack_free")
            except ValueError:
                continue
            out["tasks"].append({
                "name": " ".join(f[2:ci]),
                "cpu_permille": int(f[ci + 1]),
                "stack_free": int(f[si + 1]),
            })
        elif f[1] == "cpu":
            print("warning: device reports CPU stats unavailable "
                  "(rebuild with CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)",
                  file=sys.stderr)
    return None


def show(snap):
    h = snap["heap"]
    up = snap["uptime_ms"]
    print(f"\n--- uptime {up/1000:.1f}s ---" if up is not None else "\n---")

    for name in ("psram", "internal"):
        d = h.get(name)
        if not d:
            continue
        bits = [f"free {d.get('free', 0):>9,}", f"min {d.get('min', 0):>9,}"]
        if "largest" in d:
            bits.append(f"largest {d['largest']:>8,}")
        # "min" is the all-time low since boot -- the number that actually
        # bounds headroom, and the one MEM cannot report.
        print(f"  {name:<9} " + "  ".join(bits))

    tasks = sorted(snap["tasks"], key=lambda t: -t["cpu_permille"])
    if not tasks:
        return
    print(f"\n  {'task':<18} {'CPU':>7}  {'stack free':>11}")
    for t in tasks:
        pct = t["cpu_permille"] / 10.0
        # Stack headroom is the minimum ever seen, so a small number here
        # is a real near-miss, not a transient.
        print(f"  {t['name']:<18} {pct:>6.1f}%  {t['stack_free']:>9,} B")


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    interval = float(sys.argv[2]) if len(sys.argv) > 2 else INTERVAL_DEFAULT
    csv_path = sys.argv[3] if len(sys.argv) > 3 else None

    s = open_port()
    time.sleep(0.2)

    # Priming sample: CPU is a delta against the previous STATS call, so
    # without this the first printed sample would be a since-boot average.
    if sample(s) is None:
        sys.exit("no STATS reply -- is the firmware new enough? (MEM-only "
                 "builds predate this command)")
    time.sleep(interval)

    rows = []
    for i in range(count):
        snap = sample(s)
        if snap is None:
            print("no reply", file=sys.stderr)
        else:
            show(snap)
            for t in snap["tasks"]:
                rows.append((snap["uptime_ms"],
                             snap["heap"].get("psram", {}).get("free", 0),
                             snap["heap"].get("internal", {}).get("free", 0),
                             snap["heap"].get("internal", {}).get("largest", 0),
                             t["name"], t["cpu_permille"], t["stack_free"]))
        if i != count - 1:
            time.sleep(interval)
    s.close()

    if csv_path:
        with open(csv_path, "w") as f:
            f.write("uptime_ms,psram_free,internal_free,internal_largest,"
                    "task,cpu_permille,stack_free\n")
            for r in rows:
                f.write(",".join(str(x) for x in r) + "\n")
        print(f"\nwrote {csv_path} ({len(rows)} rows)")


if __name__ == "__main__":
    main()
