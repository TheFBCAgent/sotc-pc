"""10-second table from a SOTC_GPU_STATS log: flips/s, gpu busy, waits, VRAM over budget.

Usage: python scripts/gpustats_table.py build/game/logs/<run>.log [start_seconds]
"""
import re
import sys

ts_re = re.compile(r"^\[\s*([0-9.]+)\]")
stat_re = re.compile(
    r"gpu stats \(([0-9.]+)s\): draws (\d+) indirect (\d+) dispatches (\d+) sched-waits (\d+).*?"
    r"gpu busy (\d+) ms/s \(main (\d+), side (\d+);.*?submits (\d+), flips (\d+), "
    r"gpu-thread wait (\d+) ms/s \(other threads (\d+); side-copy fences (\d+)\)")
side_re = re.compile(r"side copies (\d+)")
heap_re = re.compile(r"heap0\(local\) usage=(\d+) budget=(\d+).*?heap1 usage=(\d+)")


def main(path, start=0.0):
    buckets = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = ts_re.match(line)
        if not m:
            continue
        t = float(m.group(1))
        if t < start:
            continue
        b = buckets.setdefault(int(t // 10) * 10, {"n": 0, "sum": [0] * 12, "side": 0, "heap": None})
        s = stat_re.search(line)
        if s:
            b["n"] += 1
            vals = [int(x) for x in s.groups()[1:]]
            for i, v in enumerate(vals):
                b["sum"][i] += v
            continue
        s = side_re.search(line)
        if s and "sched-waits by site" in line:
            b["side"] += int(s.group(1))
        h = heap_re.search(line)
        if h:
            b["heap"] = tuple(int(x) for x in h.groups())
    print(f"{'t':>5} {'flips':>6} {'busy':>5} {'main':>5} {'side':>5} {'gtwait':>6} {'other':>5} {'sidefw':>6} "
          f"{'subm':>5} {'swait':>5} {'sidecp':>6} {'draws':>6} {'disp':>6} {'h0-bud':>7} {'h1':>5}")
    for t in sorted(buckets):
        b = buckets[t]
        n = b["n"]
        if not n:
            continue
        d, ind, disp, sw, busy, mn, sd, sub, fl, gw, ow, sf = (x / n for x in b["sum"])
        heap = b["heap"]
        over = f"{heap[0] - heap[1]:+7d}" if heap else f"{'':>7}"
        h1 = f"{heap[2]:5d}" if heap else f"{'':>5}"
        print(f"{t:5d} {fl:6.1f} {busy:5.0f} {mn:5.0f} {sd:5.0f} {gw:6.0f} {ow:5.0f} {sf:6.0f} {sub:5.0f} "
              f"{sw:5.1f} {b['side'] / n:6.0f} {d + ind:6.0f} {disp:6.0f} {over} {h1}")


if __name__ == "__main__":
    main(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 0.0)
