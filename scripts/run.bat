@echo off
rem Vulkan game run for a session experiment: smoke.py with the usual setup already filled in.
rem
rem   scripts\run.bat [smoke.py options]
rem
rem Defaults (a later option on the command line wins): --seconds 600 --shots 10 --label run,
rem the Vulkan build (build-vk), the user's own dump, and the pad script that starts a new game.
rem Debug switches left set in the shell (SOTC_GPU_SKIP, SOTC_DUMP_RT, ...) are cleared first;
rem pass the ones a run needs with --env "NAME=VALUE" (repeatable). Quote it: PowerShell splits
rem an unquoted list at commas and reads a hash like 689583e5 as a number. Examples:
rem   scripts\run.bat --label dof
rem   scripts\run.bat --seconds 300 --label skip --env "SOTC_GPU_SKIP=5d6ab77,689583e5"
rem   scripts\run.bat --seconds 400 --label rtdump --env "SOTC_DUMP_RT=build/game/rt" --env "SOTC_DUMP_RT_SKIP=30" --env "SOTC_DUMP_RT_ALL=1"
setlocal
cd /d "%~dp0.."

for %%V in (SOTC_GPU_SKIP SOTC_GPU_DUMP SOTC_GPU_STATS SOTC_GPU_LOG SOTC_GPU_READBACKS
            SOTC_DUMP_RT SOTC_DUMP_RT_SKIP SOTC_DUMP_RT_ALL SOTC_GC_FREE_STALE SOTC_VK_VALIDATION
            SOTC_UNBOUNDED_CAP_MB SOTC_TIMING SOTC_STRICT SOTC_LOG SOTC_HEADLESS
            SOTC_PREFETCH_READBACK SOTC_SIDE_READBACK SOTC_HOT_FLUSH SOTC_SRT_GPU_PATCH) do set "%%V="

if not defined SOTC_GAME_DIR set "SOTC_GAME_DIR=D:\CUSA08809\uroot"
set "SOTC_PAD_SCRIPT=60:options,63:up,66:cross,85:cross,92:cross,99:cross,106:cross"

python scripts\smoke.py --exe build-vk\sotc-runtime.exe --gpu vulkan --seconds 600 --shots 10 --label run %*
exit /b %errorlevel%
