#!/usr/bin/env python3
"""Session 17: sums the SOTC_GPU_STATS `reuse` lines (in-batch drains per reader) of one or
more logs from a start time on, and prints a per-reader table and the feeding writers.

    python scripts/reuse_table.py build/game/logs/<run>.log [more logs] [--start 110]

Gap buckets: frames since the same 256-byte line / 64 KiB page last needed an in-batch drain
(new, same frame, 1, 2-3, 4-15, 16+); near = within 32 submits.
"""
import argparse
import re
from collections import defaultdict

TIME = re.compile(r"^\[\s*([\d.]+)\]")
READER = re.compile(
    r"reuse (gpu|guest) (0x[0-9a-f]+): (\d+) drains, (\d+) lines, (\d+) pages, hot (\d+), "
    r"own line in batch (\d+), no writer (\d+) \| line gaps ([\d/]+) near (\d+) \| "
    r"page gaps ([\d/]+) near (\d+) \| batch writers on the 64K \(\d+\):(.*)$")
WRITER = re.compile(r"\[(0x[0-9a-f]+) \+(0x[0-9a-f]+) (\d+)x\]")
FEEDER = re.compile(r"\[(0x[0-9a-f]+) \+(0x[0-9a-f]+) reads (\d+) dispatches (\d+)\]")
FLIPS = re.compile(r"\bflips (\d+)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--start", type=float, default=110)
    args = ap.parse_args()
    readers = defaultdict(lambda: defaultdict(int))
    gaps = defaultdict(lambda: [[0] * 6, [0] * 6])
    rwriters = defaultdict(lambda: defaultdict(int))
    feeders = defaultdict(lambda: [0, 0])
    windows = 0
    for path in args.logs:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                t = TIME.match(line)
                if not t or float(t.group(1)) < args.start:
                    continue
                if m := READER.search(line):
                    key = f"{m.group(1)} {m.group(2)}"
                    r = readers[key]
                    for name, g in zip(("drains", "lines", "pages", "hot", "own", "nowriter"),
                                       range(3, 9)):
                        r[name] += int(m.group(g))
                    r["line_near"] += int(m.group(10))
                    r["page_near"] += int(m.group(12))
                    for i, v in enumerate(m.group(9).split("/")):
                        gaps[key][0][i] += int(v)
                    for i, v in enumerate(m.group(11).split("/")):
                        gaps[key][1][i] += int(v)
                    for w in WRITER.finditer(m.group(13)):
                        rwriters[key][(w.group(1), w.group(2))] += int(w.group(3))
                elif "reuse feeders" in line:
                    windows += 1
                    for w in FEEDER.finditer(line):
                        fd = feeders[(w.group(1), w.group(2))]
                        fd[0] += int(w.group(3))
                        fd[1] += int(w.group(4))
    secs = windows * 10 or 1
    print(f"{windows} windows of 10 s from {args.start:.0f} s in {len(args.logs)} log(s)")
    print(f"{'reader':22} {'/s':>5} {'lines':>6} {'pages':>6} {'own%':>5} {'hot%':>5}  "
          f"{'line gaps new/0/1/2-3/4-15/16+ (%)':36} {'page gaps (%)':30} top batch writers")
    for key, r in sorted(readers.items(), key=lambda kv: -kv[1]["drains"]):
        n = r["drains"]
        if n == 0:
            continue
        pct = lambda g: "/".join(f"{100 * v / n:.0f}" for v in g)
        ws = sorted(rwriters[key].items(), key=lambda kv: -kv[1])[:3]
        wtxt = " ".join(f"{s}+{z} {100 * c / n:.0f}%" for (s, z), c in ws)
        print(f"{key:22} {n / secs:5.1f} {r['lines']:6} {r['pages']:6} "
              f"{100 * r['own'] / n:5.0f} {100 * r['hot'] / n:5.0f}  {pct(gaps[key][0]):36} "
              f"{pct(gaps[key][1]):30} {wtxt}")
    print("\nwriters (shader +binding size): in-batch drains fed and dispatches, per second")
    for (s, z), (reads, disp) in sorted(feeders.items(), key=lambda kv: -kv[1][0])[:12]:
        print(f"  {s} +{z:8}  reads {reads / secs:5.1f}/s  dispatches {disp / secs:6.1f}/s  "
              f"reads per dispatch {reads / disp if disp else float('nan'):.2f}")


if __name__ == "__main__":
    main()
