#!/usr/bin/env sh
# Build the boot image from a dumped game folder.
# Usage: SOTC_GAME_DIR=/path/to/CUSA08809 scripts/convert.sh
set -e
: "${SOTC_GAME_DIR:?set SOTC_GAME_DIR to the game folder (the one with eboot.bin)}"
SOTC_SHADPS4="${SOTC_SHADPS4:-../shadps4}"
cd "$(dirname "$0")/.."
NAMES=""
[ -d "$SOTC_SHADPS4/src" ] && NAMES="--shadps4 $SOTC_SHADPS4"
python3 tools/sotcconv/sotcconv.py --game "$SOTC_GAME_DIR" $NAMES -o build/game/boot.img --report build/game/boot.txt
