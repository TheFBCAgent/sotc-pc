#!/usr/bin/env sh
# Build the boot image from a dumped game folder.
# Usage: SOTC_GAME_DIR=/path/to/CUSA08809 scripts/convert.sh
set -e
: "${SOTC_GAME_DIR:?set SOTC_GAME_DIR to the game folder (the one with eboot.bin)}"
SOTC_SHADPS4="${SOTC_SHADPS4:-../shadps4}"
cd "$(dirname "$0")/.."
# Imports are resolved by name, and the names come from shadPS4's aerolib
# table: without the checkout every import stays a bare NID and none of the
# runtime's HLE functions is found.
if [ ! -f "$SOTC_SHADPS4/src/core/aerolib/aerolib.inl" ]; then
    echo "convert.sh: no shadPS4 checkout at $SOTC_SHADPS4 (set SOTC_SHADPS4; commit 14ada48)" >&2
    exit 1
fi
python3 tools/sotcconv/sotcconv.py --game "$SOTC_GAME_DIR" --shadps4 "$SOTC_SHADPS4" -o build/game/boot.img --report build/game/boot.txt
