#!/usr/bin/env bash
# GPU-thread CPU per flip for a Linux run (session 33; method in CLAUDE.md, "Linux GPU-thread
# profiles"). Starts scripts/run-vk.sh, reads utime+stime of the GPU threads at FROM and TO
# seconds, counts the `videoout: flip` lines in between and prints flips/s and CPU per flip:
# shadPS4:GpuComm (stage A, or the whole GPU thread) + sotc:GpuRecord (stage B with
# SOTC_GPU_PIPE=1, session 44; 0 otherwise). With the pipe on, flips/s is the headline.
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
cpu() { # "<GpuComm ticks> <GpuRecord ticks>"
    local a="" b=0
    for t in /proc/$pid/task/*; do
        case "$(cat "$t/comm" 2>/dev/null)" in
            shadPS4:GpuComm) a=$(awk '{print $14+$15}' "$t/stat") ;;
            sotc:GpuRecord) b=$(awk '{print $14+$15}' "$t/stat") ;;
        esac
    done
    [ -n "$a" ] && echo "$a $b"
}
wait_until() { while awk -v a="$(date +%s.%N)" -v b="$t0" -v s="$1" 'BEGIN{exit !(a-b<s)}'; do sleep 0.2; done; }
wait_until "$from"; c1=$(cpu)
wait_until "$to"; c2=$(cpu)
wait
log=$(ls -t build/game/logs/*-"$label".log | head -1)
[ -n "$c1" ] && [ -n "$c2" ] || { echo "$label: the game exited early, see $log"; exit 1; }
read -r a1 b1 <<< "$c1"; read -r a2 b2 <<< "$c2"
awk -v f="$from" -v t="$to" -v a="$((a2 - a1))" -v b="$((b2 - b1))" -v tck="$tck" -v l="$label" '
    /videoout: flip/ { s = substr($0, 2, 10) + 0; if (s >= f && s < t) n++ }
    END { ms = 1000 / tck / n
          printf "%s: %.2f flips/s, GPU-thread CPU %.2f ms/flip (A %.2f + B %.2f) (%d flips, %d-%d s)\n",
                 l, n / (t - f), (a + b) * ms, a * ms, b * ms, n, f, t }' "$log"
