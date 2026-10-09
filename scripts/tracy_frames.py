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

    python scripts/tracy_frames.py x.tracy --per-draw   # session 20

Uses `tracy-csvexport -e` (self time aggregated per zone, all threads) instead: cheap for captures
with the fine zones inside Draw/Dispatch (tens of millions of events), and prints each zone's
calls and self time per frame and per draw/dispatch (the four Rasterizer::Draw*/Dispatch* zones).
Zones with the same name at several source lines are summed.
"""
import argparse
import collections
import csv
import io
import os
import subprocess
import sys

WAITS = ("Scheduler::Wait", "SotcSideCopy fence")
RECORDS = ("Rasterizer::Draw", "Rasterizer::DrawIndirect", "Rasterizer::DispatchDirect",
           "Rasterizer::DispatchIndirect")


def per_draw(path, csvexport, top):
    text = subprocess.run([csvexport, "-e", path], check=True, capture_output=True,
                          text=True).stdout
    self_ns, calls = collections.Counter(), collections.Counter()
    for r in csv.DictReader(io.StringIO(text)):
        name = r["name"]
        if name.startswith("guest "):
            continue
        self_ns[name] += int(r["total_ns"])
        calls[name] += int(r["counts"])
    frames = max(calls["SubmitFlip"] - 1, 1)
    records = {n: c for n, c in calls.items() if n.endswith(RECORDS)}
    nrec = sum(records.values())
    if not nrec:
        sys.exit("no Rasterizer::Draw*/Dispatch* zones")
    print(f"{frames} frames; per frame: " +
          ", ".join(f"{n.rsplit(':', 1)[-1]} {c / frames:.0f}" for n, c in sorted(records.items())))
    rec_self = sum(self_ns[n] for n in records)
    print(f"{nrec / frames:.0f} draws+dispatches/frame" + chr(10))
    print(f"{'self ms/frame':>13} {'us/record':>9} {'calls/rec':>9} {'ns/call':>8}  zone")
    for n, ns in self_ns.most_common(top):
        print(f"{ns / frames / 1e6:13.2f} {ns / nrec / 1e3:9.2f} {calls[n] / nrec:9.2f} "
              f"{ns / max(calls[n], 1):8.0f}  {n}")
    print(chr(10) + f"(Draw*/Dispatch* own self time: {rec_self / nrec / 1e3:.2f} us/record; self times "
          f"sum all threads, e.g. GPU page faults on guest threads too)")


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
    ap.add_argument("--per-draw", action="store_true",
                    help="aggregated self time per zone, per draw/dispatch (csvexport -e)")
    a = ap.parse_args()
    if a.per_draw:
        per_draw(a.trace, a.csvexport, max(a.top, 40))
        return

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
