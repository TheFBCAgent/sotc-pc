#!/usr/bin/env bash
# Vulkan game run for a session experiment (Linux): smoke.py with the usual setup already filled
# in, like scripts/run.bat on Windows.
#
#   scripts/run-vk.sh [smoke.py options]
#
# Defaults (a later option on the command line wins): --seconds 600 --shots 10 --label run,
# the Vulkan build (build-vk), the user's own dump, and the pad script that starts a new game.
# Debug switches left set in the shell (SOTC_GPU_SKIP, SOTC_DUMP_RT, ...) are cleared first;
# pass the ones a run needs with --env NAME=VALUE (repeatable). Examples:
#   scripts/run-vk.sh --label dof
#   scripts/run-vk.sh --seconds 120 --label menu --env SOTC_HEADLESS=1
#   scripts/run-vk.sh --seconds 300 --label skip --env SOTC_GPU_SKIP=5d6ab77,689583e5
set -euo pipefail
cd "$(dirname "$0")/.."

for v in SOTC_GPU_SKIP SOTC_GPU_DUMP SOTC_GPU_STATS SOTC_GPU_LOG SOTC_GPU_READBACKS \
         SOTC_DUMP_RT SOTC_DUMP_RT_SKIP SOTC_DUMP_RT_ALL SOTC_GC_FREE_STALE SOTC_VK_VALIDATION \
         SOTC_UNBOUNDED_CAP_MB SOTC_TIMING SOTC_STRICT SOTC_LOG SOTC_HEADLESS \
         SOTC_PREFETCH_READBACK SOTC_SIDE_READBACK SOTC_HOT_FLUSH SOTC_HOT_AGING SOTC_SRT_GPU_PATCH \
         SOTC_ASYNC_IMAGE_FREE SOTC_TRIM_WINDOW SOTC_TEX_DOWNSCALE SOTC_FINE_TICKS \
         SOTC_CPU_COPY SOTC_CPU_COPY_LOG SOTC_GPU_PIPELINE_CACHE SOTC_STAGING_RING \
         SOTC_ASYNC_PIPELINES SOTC_PRELOAD_THREADS SOTC_GPU_PIPE SOTC_GPU_PIPE_DEPTH \
         SOTC_GPU_PIPE_VERIFY SOTC_WRITE_MEMO; do
    unset "$v"
done

export SOTC_GAME_DIR="${SOTC_GAME_DIR:-$HOME/Desktop/CUSA08809/uroot}"
export SOTC_PAD_SCRIPT="60:options,63:up,66:cross,85:cross,92:cross,99:cross,106:cross"

exec python3 scripts/smoke.py --exe build-vk/sotc-runtime --gpu vulkan --seconds 600 --shots 10 \
    --label run "$@"
