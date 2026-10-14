#!/usr/bin/env bash
# Play the game (Linux): no pad script, no screenshots, no time limit, with the recommended
# settings on. The window takes your gamepad and keyboard (README.md, "Playing"); close it to
# quit. Each session's log is kept in build/game/logs/<time>-play.log for bug reports.
#
#   scripts/play.sh [--pro] [smoke.py options]
#
# Defaults: the Vulkan build (build-vk), your dump (SOTC_GAME_DIR), save data in build/user,
# base PS4 mode (the game's own 30 fps cap) and the two-stage GPU pipeline (on by default since
# session 53; --env SOTC_GPU_PIPE=0 turns it off). --pro runs PS4 Pro mode (SOTC_NEO=1: no 30 fps cap,
# experimental). Debug switches left set in the shell are cleared first, as in run-vk.sh;
# add your own with --env NAME=VALUE (repeatable).
set -euo pipefail
cd "$(dirname "$0")/.."

for v in SOTC_GPU_SKIP SOTC_GPU_DUMP SOTC_GPU_STATS SOTC_GPU_LOG SOTC_GPU_READBACKS \
         SOTC_DUMP_RT SOTC_DUMP_RT_SKIP SOTC_DUMP_RT_ALL SOTC_GC_FREE_STALE SOTC_VK_VALIDATION \
         SOTC_UNBOUNDED_CAP_MB SOTC_TIMING SOTC_STRICT SOTC_LOG SOTC_HEADLESS \
         SOTC_PREFETCH_READBACK SOTC_SIDE_READBACK SOTC_HOT_FLUSH SOTC_HOT_AGING SOTC_HOT_BUDGET SOTC_SRT_GPU_PATCH \
         SOTC_ASYNC_IMAGE_FREE SOTC_TRIM_WINDOW SOTC_TEX_DOWNSCALE SOTC_FINE_TICKS \
         SOTC_CPU_COPY SOTC_CPU_COPY_LOG SOTC_GPU_PIPELINE_CACHE SOTC_STAGING_RING \
         SOTC_ASYNC_PIPELINES SOTC_PRELOAD_THREADS SOTC_GPU_PIPE SOTC_GPU_PIPE_DEPTH \
         SOTC_GPU_PIPE_VERIFY SOTC_WRITE_MEMO SOTC_COPY_FAST SOTC_ASYNC_READBACK \
         SOTC_PAD_SCRIPT SOTC_SHOTS SOTC_NEO SOTC_GUEST_ARGS SOTC_CALLER_TRACE \
         SOTC_ABSOLUTE_INSTANCE_ID; do
    unset "$v"
done

export SOTC_GAME_DIR="${SOTC_GAME_DIR:-$HOME/Desktop/CUSA08809/uroot}"
neo=0
args=()
for a in "$@"; do
    if [ "$a" = --pro ]; then neo=1; else args+=("$a"); fi
done
export SOTC_NEO=$neo

# smoke.py stops the game after --seconds: ~35 days here (Windows caps a wait at ~49 days).
exec python3 scripts/smoke.py --exe build-vk/sotc-runtime --gpu vulkan --seconds 3000000 \
    --label play "${args[@]}"
