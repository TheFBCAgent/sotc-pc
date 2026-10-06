#!/usr/bin/env sh
# Run the game from the boot image. Extra arguments are passed to the runtime.
# Usage: SOTC_GAME_DIR=/path/to/CUSA08809 scripts/run.sh
set -e
: "${SOTC_GAME_DIR:?set SOTC_GAME_DIR to the game folder (the one with eboot.bin)}"
cd "$(dirname "$0")/.."
export SOTC_USER_DIR="${SOTC_USER_DIR:-build/user}"
exec build/sotc-runtime build/game/boot.img "$@"
