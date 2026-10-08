#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Per-frame time breakdown of a Tracy capture (session 18).

    python scripts/tracy_frames.py build/game/tracy/altar.tracy [--csvexport D:/tracy/tracy-csvexport.exe]
    python scripts/tracy_frames.py zones.csv          # an existing `tracy-csvexport -u` export

Runs `tracy-csvexport -u` (one row per zone event), rebuilds the zone nesting per thread and
prints, per thread, the self time of every zone name in ms per frame (frames = `SubmitFlip`
zones, one per guest flip) plus the time outside any zone. Then, for the GPU thread, where its
waits sit (the outermost zone around each `Scheduler::Wait` / `SotcSideCopy fence`) and the PM4
self time (parsing and everything not covered by a child zone).
"""
import argparse
import collections
import csv
import io
import os
import subprocess
import sys

WAITS = ("Scheduler::Wait", "SotcSideCopy fence")


def load(path, csvexport):
    if path.endswith(".csv"):
        text = open(path, encoding="utf-8").read()
    else:
        text = subprocess.run([csvexport, "-u", path], check=True, capture_output=True,
                              text=True).stdout
    rows = []
    for r in csv.DictReader(io.StringIO(text)):
        start = int(r["ns_since_start"])
        rows.append((r["thread"], start, start + int(r["exec_time_ns"]), r["name"]))
    return rows


def nest(events):
    """events of one thread -> list of (name, start, end, self_ns, ancestors)."""
    events.sort(key=lambda e: (e[0], -e[1]))
    out, stack = [], []  # stack entries: [name, start, end, child_ns, ancestors]
    def pop():
        n, s, e, child, anc = stack.pop()
        out.append((n, s, e, e - s - child, anc))
    for s, e, name in events:
        while stack and stack[-1][2] <= s:
            pop()
        if stack:
            stack[-1][3] += e - s
        stack.append([name, s, e, 0, tuple(x[0] for x in stack)])
    while stack:
        pop()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--csvexport", default="D:/tracy/tracy-csvexport.exe")
    ap.add_argument("--top", type=int, default=14)
    a = ap.parse_args()

    rows = load(a.trace, a.csvexport)
    by_thread = collections.defaultdict(list)
    for t, s, e, n in rows:
        by_thread[t].append((s, e, n))
    flips = sorted(s for t, s, e, n in rows if n == "SubmitFlip")
    if len(flips) < 2:
        sys.exit("fewer than 2 SubmitFlip zones")
    lo, hi = flips[0], flips[-1]
    frames = len(flips) - 1
    span = (hi - lo) / 1e9
    print(f"{frames} frames in {span:.1f} s = {frames / span:.2f} fps, "
          f"{(hi - lo) / frames / 1e6:.1f} ms/frame (first to last SubmitFlip)\n")

    def clip(s, e):
        return max(0, min(e, hi) - max(s, lo))

    gpu_thread = collections.Counter(t for t, s, e, n in rows if n.startswith("PM4 ")).most_common(1)
    gpu_thread = gpu_thread[0][0] if gpu_thread else None
    totals = []
    for t, evs in by_thread.items():
        nested = nest(evs)
        self_by = collections.Counter()
        top_ns = 0
        for n, s, e, self_ns, anc in nested:
            # the self time inside the window, scaled by the clipped fraction
            if e > s:
                self_by[n] += self_ns * clip(s, e) / (e - s)
            if not anc:
                top_ns += clip(s, e)
        totals.append((sum(self_by.values()), t, self_by, top_ns, nested))
    totals.sort(reverse=True)

    for busy, t, self_by, top_ns, nested in totals:
        if busy / frames < 0.2e6:
            continue
        tag = " (GPU thread)" if t == gpu_thread else ""
        print(f"== thread {t}{tag}: zones {top_ns / frames / 1e6:.1f} ms/frame, "
              f"outside zones {((hi - lo) - top_ns) / frames / 1e6:.1f} ms/frame")
        for n, ns in self_by.most_common(a.top):
            if ns / frames < 0.05e6:
                break
            print(f"   {ns / frames / 1e6:7.2f} ms/frame self  {n}")
        if t == gpu_thread:
            where = collections.Counter()
            for n, s, e, self_ns, anc in nested:
                if n in WAITS and not any(x in WAITS for x in anc):
                    outer = " > ".join(anc[:3]) or "(top level)"
                    where[f"{n}  in  {outer}"] += clip(s, e)
            print("   waits by outer zones:")
            for k, ns in where.most_common(10):
                print(f"   {ns / frames / 1e6:7.2f} ms/frame  {k}")
        print()


if __name__ == "__main__":
    main()
