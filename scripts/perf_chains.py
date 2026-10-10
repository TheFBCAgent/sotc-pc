#!/usr/bin/env python3
"""Per-thread call-chain views of a perf capture (session 32, Linux).

Input: `perf script -i x.data --tid <tid> -F tid,ip,sym,dso > chains.txt` (one sample per
block, leaf frame first; inlined frames are their own lines). Percentages are of the
thread's samples (its on-CPU time).

  perf_chains.py chains.txt self [N]                   top N leaf functions
  perf_chains.py chains.txt incl REGEX...              samples with a frame matching REGEX
  perf_chains.py chains.txt callers LEAF_RE [DEPTH [SKIP_RE]]
                                                        callers of matching leaves, DEPTH frames up
                                                        (frames matching SKIP_RE left out)
  perf_chains.py chains.txt children FRAME_RE          what the first matching frame calls
  perf_chains.py chains.txt from FRAME_RE [DEPTH]      the first matching frame and DEPTH-1 callers
"""
import collections
import re
import sys


def load(path):
    samples, cur = [], None
    for line in open(path, errors="replace"):
        if re.match(r"^\s*\d+\s*$", line):  # "  <tid>" starts a sample
            if cur is not None:
                samples.append(cur)
            cur = []
            continue
        parts = line.strip().split(None, 1)
        if cur is not None and len(parts) == 2:
            cur.append(re.sub(r"\+0x[0-9a-f]+ \(", " (", parts[1]))
    if cur:
        samples.append(cur)
    return samples


def short(frame):
    frame = re.sub(r"\(([^()]*)\)$", lambda m: "[" + m.group(1).split("/")[-1] + "]", frame)
    for _ in range(3):
        frame = re.sub(r"<[^<>]*>", "", frame)
    for _ in range(2):
        frame = re.sub(r"\([^()]*\)", "", frame)
    return frame[:110]


def show(counter, n, total, limit=15):
    for key, v in counter.most_common(limit):
        print(f"{100 * v / total:5.1f}% {key}")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    samples, mode, args = load(sys.argv[1]), sys.argv[2], sys.argv[3:]
    n = len(samples) or 1
    print(f"{len(samples)} samples")
    if mode == "self":
        show(collections.Counter(short(s[0]) for s in samples if s), n, n,
             int(args[0]) if args else 40)
    elif mode == "incl":
        for pat in args:
            rx = re.compile(pat)
            hit = sum(1 for s in samples if any(rx.search(f) for f in s))
            print(f"{100 * hit / n:5.1f}% {pat}")
    elif mode == "callers":
        rx = re.compile(args[0])
        depth = int(args[1]) if len(args) > 1 else 5
        skip = re.compile(args[2]) if len(args) > 2 else None
        c, tot = collections.Counter(), 0
        for s in samples:
            if s and rx.search(s[0]):
                tot += 1
                frames = [short(f) for f in s[1:]]
                if skip:
                    frames = [f for f in frames if not skip.search(f)]
                c[" <- ".join(frames[:depth])] += 1
        print(f"leaves matching: {100 * tot / n:.1f}%")
        show(c, n, n)
    elif mode in ("children", "from"):
        rx = re.compile(args[0])
        depth = int(args[1]) if len(args) > 1 else 4
        c = collections.Counter()
        for s in samples:
            for i, f in enumerate(s):
                if rx.search(f):
                    if mode == "children":
                        c[short(s[i - 1]) if i else "SELF"] += 1
                    else:
                        c[" <- ".join(short(x) for x in s[i:i + depth])] += 1
                    break
        show(c, n, n)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
