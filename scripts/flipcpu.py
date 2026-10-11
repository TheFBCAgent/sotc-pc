"""GPU-thread CPU per flip for a Windows run: scripts/flipcpu.sh's twin (session 37).

Starts scripts\\run.bat (new game through the pad script), reads the CPU time (user + kernel)
of the GPU thread (the one named *GpuComm*) at FROM and TO seconds after the runtime starts,
counts the `videoout: flip` lines in between and prints both.

    python scripts/flipcpu.py <label> [--exe build-vk\\sotc-runtime.exe] [--from 150] [--to 225]

The run lasts TO + 10 s, with one screenshot at TO + 5 s (to check the scene: with run.bat's
pad script the game is at the altar from ~150 s). Windows only lets the exes in some build
directories write outside their own tree (session 37): copy a variant into build-vk\\ first.
"""
import argparse
import glob
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
from smoke import thread_times  # noqa: E402


def find_pid(image: str) -> int | None:
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {image}", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0].lower() == image.lower():
            return int(parts[1])
    return None


def gpu_cpu(pid: int) -> float | None:
    for name, user, kernel in thread_times(pid):
        if "GpuComm" in name:
            return user + kernel
    return None


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("label")
    ap.add_argument("--exe", default=r"build-vk\sotc-runtime.exe")
    ap.add_argument("--from", dest="start", type=float, default=150)
    ap.add_argument("--to", dest="end", type=float, default=225)
    args = ap.parse_args()
    run = subprocess.Popen(
        ["cmd", "/c", r"scripts\run.bat", "--exe", args.exe, "--label", args.label,
         "--seconds", str(int(args.end + 10)), "--shots", str(int(args.end + 5))],
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    image = Path(args.exe).name
    pid = None
    for _ in range(300):
        pid = find_pid(image)
        if pid:
            break
        time.sleep(0.1)
    if not pid:
        sys.exit(f"{image} did not start")
    t0 = time.monotonic()
    time.sleep(max(0.0, args.start - (time.monotonic() - t0)))
    c1 = gpu_cpu(pid)
    time.sleep(max(0.0, args.end - (time.monotonic() - t0)))
    c2 = gpu_cpu(pid)
    run.wait()
    log = max(glob.glob(str(ROOT / "build" / "game" / "logs" / f"*-{args.label}.log")),
              key=os.path.getmtime)
    if c1 is None or c2 is None:
        sys.exit(f"{args.label}: no GPU thread or the game exited early, see {log}")
    flips = 0
    with open(log, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "videoout: flip" in line:
                s = float(line[1:line.index("]")])
                if args.start <= s < args.end:
                    flips += 1
    if not flips:
        sys.exit(f"{args.label}: no flips between {args.start} and {args.end} s, see {log}")
    print(f"{args.label}: GPU-thread CPU {(c2 - c1) * 1000 / flips:.2f} ms/flip, "
          f"{flips / (args.end - args.start):.2f} flips/s ({flips} flips, "
          f"{args.start:.0f}-{args.end:.0f} s)")


if __name__ == "__main__":
    main()
