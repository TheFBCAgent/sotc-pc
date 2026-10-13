#!/bin/bash
# Nsight Systems capture of a Windows Vulkan run (session 57): run.bat's setup (the user's dump,
# the new-game pad script, Pro mode), traced by `nsys profile`, the window from DELAY to
# DELAY+DURATION s, then the game is killed. One screenshot 2 s before the end.
#
#   scripts/nsys.sh <label> [delay=160] [duration=10] [extra nsys options, default --trace=vulkan]
#
# Writes build/nsys/<label>.nsys-rep and the game's log build/nsys/<label>.game.log (nsys does
# not pass the game's output through on Windows). Then, for example:
#   nsys stats -r vulkan_api_sum -f csv -o build/nsys/<label> build/nsys/<label>.nsys-rep
# (it also writes <label>.sqlite: the VULKAN_API table has per-thread calls; the API trace covers
# the whole run, the collection window is start >= 0). Captures are game-derived: never commit.
# Setup once, from an admin PowerShell (nsys registers its Vulkan layer in HKLM itself and fails
# without admin rights): see docs/DEBUGGING.md, "Nsight Systems".
cd "$(dirname "$0")/.."
label=$1; delay=${2:-160}; dur=${3:-10}; shift $(( $# < 3 ? $# : 3 ))
[ $# -eq 0 ] && set -- --trace=vulkan
for v in $(env | grep -o '^SOTC_[A-Z0-9_]*'); do unset "$v"; done
export SOTC_GAME_DIR="${SOTC_GAME_DIR_NSYS:-D:\CUSA08809\uroot}"
export SOTC_PAD_SCRIPT="60:options,63:up,66:cross,85:cross,92:cross,99:cross,106:cross"
export SOTC_GPU=vulkan SOTC_NEO=${NEO:-1} SOTC_USER_DIR="$(pwd -W)\build\user"
export SOTC_SHOTS=$((delay + dur - 2))
mkdir -p build/nsys
NSYS=${NSYS:-"/c/Program Files/NVIDIA Corporation/Nsight Systems 2026.5.1/target-windows-x64/nsys.exe"}
GAMELOG='build\nsys\'"$label"'.game.log'
"$NSYS" profile -o "build/nsys/$label" --force-overwrite=true --delay="$delay" --duration="$dur" \
  --kill=true --sample=none --cpuctxsw=none "$@" \
  cmd //c "build-vk\sotc-runtime.exe build\game\boot.img > $GAMELOG 2>&1" > "build/nsys/$label.log" 2>&1
code=$?
echo "nsys exit $code; report build/nsys/$label.nsys-rep, game log $GAMELOG"
exit $code
