#!/usr/bin/env bash
# GPU-thread CPU per flip for a Linux run (session 33; method in CLAUDE.md, "Linux GPU-thread
# profiles"). Starts scripts/run-vk.sh, reads utime+stime of the shadPS4:GpuComm thread at
# FROM and TO seconds, counts the `videoout: flip` lines in between and prints both.
#
#   scripts/flipcpu.sh <label> [run-vk.sh options]
#   FROM=110 TO=195 SECONDS_RUN=200 (defaults; the temple with the save continue)
set -uo pipefail
cd "$(dirname "$0")/.."
label=${1:?usage: scripts/flipcpu.sh <label> [run-vk.sh options]}; shift
from=${FROM:-110} to=${TO:-195} run=${SECONDS_RUN:-200}

scripts/run-vk.sh --seconds "$run" --label "$label" "$@" > /dev/null 2>&1 &
for _ in $(seq 100); do pid=$(pgrep -nx sotc-runtime) && break; sleep 0.1; done
[ -n "${pid:-}" ] || { echo "sotc-runtime did not start"; exit 1; }
t0=$(date +%s.%N)
tck=$(getconf CLK_TCK)
cpu() {
    for t in /proc/$pid/task/*; do
        [ "$(cat "$t/comm" 2>/dev/null)" = "shadPS4:GpuComm" ] && awk '{print $14+$15}' "$t/stat"
    done
}
wait_until() { while awk -v a="$(date +%s.%N)" -v b="$t0" -v s="$1" 'BEGIN{exit !(a-b<s)}'; do sleep 0.2; done; }
wait_until "$from"; c1=$(cpu)
wait_until "$to"; c2=$(cpu)
wait
log=$(ls -t build/game/logs/*-"$label".log | head -1)
[ -n "$c1" ] && [ -n "$c2" ] || { echo "$label: the game exited early, see $log"; exit 1; }
awk -v f="$from" -v t="$to" -v c1="$c1" -v c2="$c2" -v tck="$tck" -v l="$label" '
    /videoout: flip/ { s = substr($0, 2, 10) + 0; if (s >= f && s < t) n++ }
    END { printf "%s: GPU-thread CPU %.2f ms/flip, %.2f flips/s (%d flips, %d-%d s)\n",
                 l, (c2 - c1) * 1000 / tck / n, n / (t - f), n, f, t }' "$log"
