#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Per-thread breakdown of a Tracy capture with the fine zones on (session 32).

    python scripts/tracy_threads.py build/game/tracy/x.tracy [--csvexport D:/tracy/tracy-csvexport.exe]

A full `tracy-csvexport -u` of a capture with the session 20 zones is too big, so this exports
only the coarse zones (`-u -f <name>` per name) and:
- per thread: count and ms/frame of GPU page faults, guest waits (mutex/cond/evflag/equeue/sleep)
  and the GPU thread's PM4/wait/download zones (frames = `SubmitFlip` zones);
- for the GPU thread (the one with `SubmitFlip`): inclusive and self ms/frame of those zones
  rebuilt into a nesting, and the zone chain above every wait/download (e.g. a download straight
  under `PM4 gfx queue` is a guest fault's download queued to the GPU thread; under a
  `Rasterizer::*` zone and a `GPU page fault`, the GPU thread's own fault).
Thread ids are Tracy's indices, not names.
"""
import argparse
import collections
import csv
import io
import subprocess

ZONES = ("SubmitFlip", "PM4", "Vulkan::Rasterizer::", "Scheduler::Wait", "Scheduler::Finish",
         "SotcSideCopy fence", "ReadMemory download", "GPU page fault", "SignalImpl",
         "RefreshImage", "Image GC", "Compile shader", "guest")
WAITS = ("Scheduler::Wait", "Scheduler::Finish", "SotcSideCopy fence",
         "BufferCache::ReadMemory download", "GPU page fault")


def export(csvexport, path, name):
    text = subprocess.run([csvexport, "-u", "-f", name, path], check=True, capture_output=True,
                          text=True, errors="replace").stdout
    return list(csv.DictReader(io.StringIO(text)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--csvexport", default="D:/tracy/tracy-csvexport.exe")
    args = ap.parse_args()

    rows = {}
    for z in ZONES:
        for r in export(args.csvexport, args.trace, z):
            if not (r["ns_since_start"] or "").isdigit():
                continue  # a zone name with a comma shifts the columns
            # The filters overlap (substring match): key each event by name, start and thread.
            rows[(r["name"], r["ns_since_start"], r["thread"])] = r
    flips = sorted(int(r["ns_since_start"]) for r in rows.values() if r["name"] == "SubmitFlip")
    gpu_thread = next(r["thread"] for r in rows.values() if r["name"] == "SubmitFlip")
    t0, t1, nf = flips[0], flips[-1], len(flips) - 1
    print(f"{nf} frames, {(t1 - t0) / nf / 1e6:.2f} ms/frame; GPU thread = t{gpu_thread}")

    per = collections.defaultdict(lambda: [0, 0])
    ev = []
    for r in rows.values():
        s = int(r["ns_since_start"])
        if s < t0 or s >= t1:
            continue
        d = int(r["exec_time_ns"])
        name = r["name"].replace("Vulkan::Rasterizer::", "R::")
        if not name.startswith("R::"):
            per[(r["thread"], name)][0] += 1
            per[(r["thread"], name)][1] += d
        if r["thread"] == gpu_thread:
            ev.append((s, -d, name))

    print("\nper thread (count/frame, ms/frame):")
    for (t, n), (c, d) in sorted(per.items(), key=lambda x: (int(x[0][0]), -x[1][1])):
        if d / nf >= 10_000:  # >= 0.01 ms/frame
            print(f"  t{t:>3}  {n:40s} {c / nf:9.1f} {d / nf / 1e6:8.2f}")

    ev.sort()  # by start, longer (outer) zones first
    incl, selfsum, cnt, chains = (collections.Counter() for _ in range(4))
    stack = []  # (end, name)
    for s, negd, n in ev:
        d = -negd
        while stack and stack[-1][0] <= s:
            stack.pop()
        if stack:
            selfsum[stack[-1][1]] -= d  # self = inclusive minus direct children
            if n in WAITS:
                chains[(n, " > ".join(x[1] for x in stack))] += d
        stack.append((s + d, n))
        incl[n] += d
        selfsum[n] += d
        cnt[n] += 1
    print(f"\nGPU thread t{gpu_thread} (ms/frame: inclusive, self; count/frame):")
    for n in sorted(incl, key=lambda k: -incl[k]):
        print(f"  {n:40s} {incl[n] / nf / 1e6:8.2f} {selfsum[n] / nf / 1e6:8.2f} {cnt[n] / nf:9.1f}")
    print("\nGPU thread waits/downloads by zone chain (ms/frame):")
    for (n, c), d in sorted(chains.items(), key=lambda x: -x[1])[:20]:
        if d / nf >= 10_000:
            print(f"  {d / nf / 1e6:7.2f}  {n}  <=  {c}")


if __name__ == "__main__":
    main()
