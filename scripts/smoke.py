#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the game for a while and summarize how far it got.

Usage:
    python scripts/smoke.py [--seconds 60] [--strict] [--log-level 2]
Needs SOTC_GAME_DIR (the folder with eboot.bin) and build/game/boot.img
(scripts/convert.*). The full log goes to build/game/smoke.log.

Summary:
  - result: exited / crashed / still running at timeout (hang or main loop)
  - frames: highest "videoout: frame N" logged (VideoOut logs every 60 frames)
  - unimplemented imports in first-call order (missing HLE functions)
  - intentional stubs hit (HLE_STUB_OK)
  - crash report and the last guest stdout lines
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--strict", action="store_true", help="stop at the first unimplemented import")
    ap.add_argument("--log-level", default=None, help="SOTC_LOG (default: inherit or 2)")
    ap.add_argument("--image", type=Path, default=ROOT / "build" / "game" / "boot.img")
    args = ap.parse_args()

    if not os.environ.get("SOTC_GAME_DIR"):
        print("smoke: set SOTC_GAME_DIR to the game folder (the one with eboot.bin)")
        return 2
    exe = ROOT / "build" / ("sotc-runtime.exe" if os.name == "nt" else "sotc-runtime")
    if not exe.exists() or not args.image.exists():
        print(f"smoke: need {exe} (cmake --build build) and {args.image} (scripts/convert.*)")
        return 2

    env = dict(os.environ)
    env.setdefault("SOTC_USER_DIR", str(ROOT / "build" / "user"))
    if args.strict:
        env["SOTC_STRICT"] = "1"
    if args.log_level is not None:
        env["SOTC_LOG"] = args.log_level

    log_path = ROOT / "build" / "game" / "smoke.log"
    with open(log_path, "wb") as log:
        proc = subprocess.Popen([str(exe), str(args.image)], stdout=log, stderr=subprocess.STDOUT,
                                env=env, cwd=ROOT)
        try:
            code = proc.wait(timeout=args.seconds)
            result = f"exited with code {code}" + (" (crash)" if code == 3 else "")
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            result = f"still running after {args.seconds:.0f}s (hang or main loop: check frames)"

    text = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
    unimplemented, stubs, frames, crash, stdout = [], [], 0, [], []
    for line in text:
        if m := re.search(r"unimplemented (\S+)", line):
            if m.group(1) not in unimplemented:
                unimplemented.append(m.group(1))
        elif m := re.search(r"stub (\S+) -> 0", line):
            if m.group(1) not in stubs:
                stubs.append(m.group(1))
        if m := re.search(r"videoout: frame (\d+)", line):
            frames = max(frames, int(m.group(1)))
        if re.search(r"(access violation|illegal instruction|fatal exception) at|stack\[|"
                     r"faulting address|ERROR", line):
            crash.append(line)
        if m := re.search(r"guest (stdout|stderr): (.*)", line):
            if not stdout or stdout[-1] != m.group(2):
                stdout.append(m.group(2))

    print(f"result:  {result}")
    print(f"frames:  {frames}")
    print(f"log:     {log_path} ({len(text)} lines)")
    print(f"unimplemented ({len(unimplemented)}, first-call order):")
    for name in unimplemented:
        print(f"  {name}")
    if stubs:
        print(f"intentional stubs hit ({len(stubs)}): {', '.join(stubs)}")
    if crash:
        print("errors / crash:")
        for line in crash[-16:]:
            print(f"  {line}")
    print("last guest output:")
    for line in stdout[-10:]:
        print(f"  {line[:200]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
