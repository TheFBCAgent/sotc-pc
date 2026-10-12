@echo off
rem Play the game (Windows): no pad script, no screenshots, no time limit, with the recommended
rem settings on. The window takes your gamepad and keyboard (README.md, "Playing"); close it to
rem quit. Each session's log is kept in build\game\logs\<time>-play.log for bug reports.
rem
rem   scripts\play.bat [--pro] [smoke.py options]
rem
rem Defaults: the Vulkan build (build-vk), your dump (SOTC_GAME_DIR), save data in build\user,
rem base PS4 mode (the game's own 30 fps cap) and the two-stage GPU pipeline (SOTC_GPU_PIPE=1;
rem not yet measured on Windows: if it misbehaves, add --env "SOTC_GPU_PIPE=0"). --pro (first
rem argument) runs PS4 Pro mode (SOTC_NEO=1: no 30 fps cap, experimental). Debug switches left
rem set in the shell are cleared first, as in run.bat; add your own with --env "NAME=VALUE".
setlocal
cd /d "%~dp0.."

for %%V in (SOTC_GPU_SKIP SOTC_GPU_DUMP SOTC_GPU_STATS SOTC_GPU_LOG SOTC_GPU_READBACKS
            SOTC_DUMP_RT SOTC_DUMP_RT_SKIP SOTC_DUMP_RT_ALL SOTC_GC_FREE_STALE SOTC_VK_VALIDATION
            SOTC_UNBOUNDED_CAP_MB SOTC_TIMING SOTC_STRICT SOTC_LOG SOTC_HEADLESS
            SOTC_PREFETCH_READBACK SOTC_SIDE_READBACK SOTC_HOT_FLUSH SOTC_HOT_AGING SOTC_HOT_BUDGET SOTC_SRT_GPU_PATCH
            SOTC_ASYNC_IMAGE_FREE SOTC_TRIM_WINDOW SOTC_TEX_DOWNSCALE SOTC_FINE_TICKS
            SOTC_CPU_COPY SOTC_CPU_COPY_LOG SOTC_GPU_PIPELINE_CACHE SOTC_STAGING_RING
            SOTC_ASYNC_PIPELINES SOTC_PRELOAD_THREADS SOTC_RS_MEMO
            SOTC_GPU_PIPE SOTC_GPU_PIPE_DEPTH SOTC_GPU_PIPE_VERIFY SOTC_WRITE_MEMO SOTC_COPY_FAST SOTC_ASYNC_READBACK
            SOTC_PAD_SCRIPT SOTC_SHOTS SOTC_NEO) do set "%%V="

if not defined SOTC_GAME_DIR set "SOTC_GAME_DIR=D:\CUSA08809\uroot"
set "SOTC_GPU_PIPE=1"
set "SOTC_NEO=0"
if /i "%~1"=="--pro" (
    set "SOTC_NEO=1"
    shift
)

rem smoke.py stops the game after --seconds: ~35 days here (Windows caps a wait at ~49 days).
python scripts\smoke.py --exe build-vk\sotc-runtime.exe --gpu vulkan --seconds 3000000 --label play %1 %2 %3 %4 %5 %6 %7 %8 %9
exit /b %errorlevel%
