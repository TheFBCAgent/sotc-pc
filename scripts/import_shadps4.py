#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Vendor the parts of shadPS4 (GPL-2.0) that sotcgpu builds from.

Copies src/common, src/video_core, src/shader_recompiler and LICENSE from a
shadPS4 git checkout at a fixed commit into third_party/shadps4/, using
`git archive`, so the result is the commit's exact contents whatever state
the checkout's working tree is in.

    python scripts/import_shadps4.py [--src ../shadps4] [--commit 14ada48]

Re-running replaces the vendored tree, so local patches (listed in
third_party/shadps4/README.md) are lost. Re-apply them afterwards.
"""
import argparse
import io
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PATHS = ["src/common", "src/video_core", "src/shader_recompiler", "LICENSE"]
KEEP = {"README.md"}  # our own files in the destination


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--src", default=str(ROOT.parent / "shadps4"), help="shadPS4 git checkout")
    ap.add_argument("--commit", default="14ada48", help="commit to vendor")
    ap.add_argument("--dest", default=str(ROOT / "third_party" / "shadps4"))
    args = ap.parse_args()

    src, dest = Path(args.src), Path(args.dest)
    full = subprocess.run(["git", "-C", str(src), "rev-parse", args.commit + "^{commit}"],
                          capture_output=True, text=True)
    if full.returncode:
        print(f"error: {args.commit} not found in {src}: {full.stderr.strip()}", file=sys.stderr)
        return 1
    commit = full.stdout.strip()
    tar = subprocess.run(["git", "-C", str(src), "archive", "--format=tar", commit, *PATHS],
                         capture_output=True, check=True).stdout

    dest.mkdir(parents=True, exist_ok=True)
    for child in dest.iterdir():
        if child.name in KEEP:
            continue
        shutil.rmtree(child) if child.is_dir() else child.unlink()
    count = 0
    with tarfile.open(fileobj=io.BytesIO(tar)) as tf:
        for member in tf.getmembers():
            if not member.isfile():
                continue
            out = dest / member.name
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(tf.extractfile(member).read())
            count += 1
    print(f"vendored {count} files from shadPS4 {commit[:12]} into {dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
