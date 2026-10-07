#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the game for a while and summarize how far it got.

Usage:
    python scripts/smoke.py [--seconds 60] [--strict] [--log-level 2]
                            [--gpu null|vulkan] [--shots N] [--threads] [--timing]
Needs SOTC_GAME_DIR (the folder with eboot.bin) and build/game/boot.img
(scripts/convert.*). The full log goes to build/game/smoke.log.

Summary:
  - result: exited / crashed / still running at timeout (hang or main loop)
  - frames: highest "videoout: frame N" logged (VideoOut logs every 60 frames)
  - flips/s: mean and 1% low over the run (see flip_report)
  - unimplemented imports in first-call order (missing HLE functions)
  - intentional stubs hit (HLE_STUB_OK)
  - crash report and the last guest stdout lines
Options:
  --gpu      sets SOTC_GPU (null or vulkan); default: inherit / the runtime's choice
  --shots N  sets SOTC_SHOTS=N (a screenshot every N s into build/game/shots/)
  --threads  (Windows) per-thread CPU time just before the run is stopped
  --timing   sets SOTC_TIMING=1: the runtime logs its sleep / wait accounting
             every 10 s; the last table is printed
  --exe      the runtime to run (default build/sotc-runtime[.exe]), e.g. to compare builds
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Log lines: "[  12.3456] INFO  t1234  message" (seconds since start).
LOG_TIME = re.compile(r"^\[\s*(\d+\.\d+)\]")
# Every 60 flips (videoout.c today).
FRAME_LINE = re.compile(r"videoout: frame (\d+)")
# Per flip, when videoout.c prints it (requested in docs/m3/p.md):
#   "videoout: flip <count> at <process_time_us> us"
FLIP_LINE = re.compile(r"videoout: flip (\d+) at (\d+) us")


def percentile_low(frame_times: list[float], fraction: float = 0.01) -> float:
    """Average rate over the slowest `fraction` of frames ("1% low"), in frames/s."""
    worst = sorted(frame_times, reverse=True)[: max(1, int(len(frame_times) * fraction))]
    return len(worst) / sum(worst) if sum(worst) > 0 else 0.0


def flip_report(lines: list[str], skip_s: float) -> list[str]:
    """Flips per second from the log.

    With per-flip lines (FLIP_LINE) the 1% low is exact: the mean rate over the
    slowest 1% of flip intervals. Without them it falls back to the 60-flip
    "frame N" lines: the mean is exact, and the low is the slowest 60-flip
    window (it hides any single long frame inside a window). Flips in the first
    `skip_s` seconds (boot, loading) are left out of the steady-state figures.
    """
    flips, frames = [], []
    for line in lines:
        if m := FLIP_LINE.search(line):
            t = LOG_TIME.match(line)
            # (count, precise time s, log time s: used only for the boot skip)
            flips.append((int(m.group(1)), int(m.group(2)) / 1e6, float(t.group(1)) if t else 0.0))
        elif (m := FRAME_LINE.search(line)) and (t := LOG_TIME.match(line)):
            frames.append((int(m.group(1)), float(t.group(1)), float(t.group(1))))
    out = []

    def summarize(points: list[tuple[int, float, float]], per_flip: bool) -> None:
        if len(points) < 2:
            return
        n0, t0 = points[0][:2]
        n1, t1 = points[-1][:2]
        total = f"{(n1 - n0) / (t1 - t0):.2f}/s over {t1 - t0:.1f} s ({n1 - n0} flips)"
        steady = [p for p in points if p[-1] >= skip_s]
        if len(steady) >= 2:
            s0, s1 = steady[0], steady[-1]
            mean = (s1[0] - s0[0]) / (s1[1] - s0[1]) if s1[1] > s0[1] else 0.0
            intervals = [(b[1] - a[1]) / (b[0] - a[0]) for a, b in zip(steady, steady[1:])
                         if b[0] > a[0]]
            if per_flip:
                low = percentile_low(intervals)
                worst = max(intervals) * 1000
                lowtxt = f"1% low {low:.2f}/s, worst frame {worst:.1f} ms"
            else:
                low = 1 / max(intervals)
                lowtxt = f"slowest 60-flip window {low:.2f}/s (no per-flip log: not a true 1% low)"
            out.append(f"mean {mean:.2f}/s after {skip_s:.0f} s, {lowtxt}")
            # Per 10 s, to show phases (loading at 60 Hz, gameplay at 30 Hz...).
            buckets: dict[int, list[tuple[int, float, float]]] = {}
            for p in points:
                buckets.setdefault(int(p[1] // 10), []).append(p)
            rates = []
            for k in sorted(buckets):
                b = buckets[k]
                if len(b) >= 2 and b[-1][1] > b[0][1]:
                    rates.append(f"{(b[-1][0] - b[0][0]) / (b[-1][1] - b[0][1]):.1f}")
            out.append("per 10 s: " + " ".join(rates))
        out.insert(0, f"mean {total}")

    if len(flips) >= 2:
        summarize(flips, True)
    else:
        summarize(frames, False)
    return out


def thread_times(pid: int) -> list[tuple[str, float, float]]:
    """(name, user s, kernel s) for every thread of `pid` (Windows only)."""
    if os.name != "nt":
        return []
    import ctypes
    from ctypes import wintypes as wt

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)

    class THREADENTRY32(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                    ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", wt.LONG),
                    ("tpDeltaPri", wt.LONG), ("dwFlags", wt.DWORD)]

    k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
    k32.OpenThread.restype = wt.HANDLE
    k32.GetThreadDescription.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_wchar_p)]
    k32.LocalFree.argtypes = [ctypes.c_void_p]
    snap = k32.CreateToolhelp32Snapshot(0x4, 0)  # TH32CS_SNAPTHREAD
    entry = THREADENTRY32()
    entry.dwSize = ctypes.sizeof(entry)
    tids = []
    ok = k32.Thread32First(snap, ctypes.byref(entry))
    while ok:
        if entry.th32OwnerProcessID == pid:
            tids.append(entry.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(entry))
    k32.CloseHandle(snap)
    result = []
    for tid in tids:
        h = k32.OpenThread(0x1000 | 0x0800, False, tid)  # QUERY_LIMITED_INFORMATION | QUERY_INFORMATION
        if not h:
            continue
        c, e, kt, ut = (wt.FILETIME() for _ in range(4))
        name = ""
        desc = ctypes.c_wchar_p()
        if k32.GetThreadDescription(h, ctypes.byref(desc)) >= 0 and desc.value:
            name = desc.value
        if desc:
            k32.LocalFree(ctypes.cast(desc, ctypes.c_void_p))
        if k32.GetThreadTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt),
                              ctypes.byref(ut)):
            def sec(ft: wt.FILETIME) -> float:
                return ((ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 1e7
            result.append((name or f"tid {tid}", sec(ut), sec(kt)))
        k32.CloseHandle(h)
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--strict", action="store_true", help="stop at the first unimplemented import")
    ap.add_argument("--log-level", default=None, help="SOTC_LOG (default: inherit or 2)")
    ap.add_argument("--image", type=Path, default=ROOT / "build" / "game" / "boot.img")
    ap.add_argument("--exe", type=Path, default=None, help="runtime executable to run")
    ap.add_argument("--gpu", choices=["null", "vulkan"], default=None, help="sets SOTC_GPU")
    ap.add_argument("--shots", type=float, default=None, metavar="N",
                    help="sets SOTC_SHOTS=N: a screenshot every N s into build/game/shots/")
    ap.add_argument("--threads", action="store_true",
                    help="report per-thread CPU time at the end (Windows)")
    ap.add_argument("--timing", action="store_true",
                    help="SOTC_TIMING=1: print the runtime's last wait-accounting table")
    ap.add_argument("--skip", type=float, default=5.0,
                    help="seconds of boot left out of the steady flip rate (default 5)")
    args = ap.parse_args()

    if not os.environ.get("SOTC_GAME_DIR"):
        print("smoke: set SOTC_GAME_DIR to the game folder (the one with eboot.bin)")
        return 2
    exe = args.exe or ROOT / "build" / ("sotc-runtime.exe" if os.name == "nt" else "sotc-runtime")
    if not exe.exists() or not args.image.exists():
        print(f"smoke: need {exe} (cmake --build build) and {args.image} (scripts/convert.*)")
        return 2

    env = dict(os.environ)
    env.setdefault("SOTC_USER_DIR", str(ROOT / "build" / "user"))
    if args.strict:
        env["SOTC_STRICT"] = "1"
    if args.log_level is not None:
        env["SOTC_LOG"] = args.log_level
    if args.gpu:
        env["SOTC_GPU"] = args.gpu
    if args.timing:
        env["SOTC_TIMING"] = "1"
    shots_dir = ROOT / "build" / "game" / "shots"
    shots_before = set()
    if args.shots is not None:
        env["SOTC_SHOTS"] = str(args.shots)
        if shots_dir.is_dir():
            shots_before = {p.name for p in shots_dir.glob("*.png")}

    log_path = ROOT / "build" / "game" / "smoke.log"
    threads = []
    with open(log_path, "wb") as log:
        proc = subprocess.Popen([str(exe), str(args.image)], stdout=log, stderr=subprocess.STDOUT,
                                env=env, cwd=ROOT)
        try:
            code = proc.wait(timeout=args.seconds)
            result = f"exited with code {code}" + (" (crash)" if code == 3 else "")
        except subprocess.TimeoutExpired:
            if args.threads:
                threads = thread_times(proc.pid)
            proc.kill()
            proc.wait()
            result = f"still running after {args.seconds:.0f}s (hang or main loop: check frames)"

    text = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
    unimplemented, stubs, frames, crash, stdout, timing = [], [], 0, [], [], []
    for line in text:
        if m := re.search(r"unimplemented (\S+)", line):
            if m.group(1) not in unimplemented:
                unimplemented.append(m.group(1))
        elif m := re.search(r"stub (\S+) -> 0", line):
            if m.group(1) not in stubs:
                stubs.append(m.group(1))
        if m := FRAME_LINE.search(line):
            frames = max(frames, int(m.group(1)))
        if re.search(r"(access violation|illegal instruction|fatal exception) at|stack\[|"
                     r"faulting address|ERROR", line):
            crash.append(line)
        if m := re.search(r"guest (stdout|stderr): (.*)", line):
            if not stdout or stdout[-1] != m.group(2):
                stdout.append(m.group(2))
        if "timing: waits at" in line:
            timing = [line]
        elif "timing:   " in line and timing:
            timing.append(line)

    print(f"result:  {result}")
    print(f"frames:  {frames}")
    for i, line in enumerate(flip_report(text, args.skip)):
        print(f"{'flips/s:' if i == 0 else '':8} {line}")
    print(f"log:     {log_path} ({len(text)} lines)")
    if args.shots is not None:
        saved = sorted(p.name for p in shots_dir.glob("*.png")) if shots_dir.is_dir() else []
        new = [s for s in saved if s not in shots_before]
        print(f"shots:   {len(new)} saved in {shots_dir}" + (f" ({new[0]} .. {new[-1]})" if new else ""))
    print(f"unimplemented ({len(unimplemented)}, first-call order):")
    for name in unimplemented:
        print(f"  {name}")
    if stubs:
        print(f"intentional stubs hit ({len(stubs)}): {', '.join(stubs)}")
    if crash:
        print(f"errors / crash ({len(crash)} lines, last 16):")
        for line in crash[-16:]:
            print(f"  {line}")
    print("last guest output:")
    for line in stdout[-10:]:
        print(f"  {line[:200]}")
    if threads:
        total = sum(u + k for _, u, k in threads)
        print(f"thread CPU time ({len(threads)} threads, {total:.1f} s total over "
              f"{args.seconds:.0f} s; top 16):")
        for name, u, k in sorted(threads, key=lambda t: -(t[1] + t[2]))[:16]:
            print(f"  {name[:32]:32} {u + k:7.1f} s ({100 * (u + k) / args.seconds:5.1f}% of a core,"
                  f" kernel {k:5.1f} s)")
    if timing:
        print("wait accounting (SOTC_TIMING, runtime call sites):")
        for line in timing:
            print(f"  {line.split('timing: ', 1)[-1].strip()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
