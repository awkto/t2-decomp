#!/bin/sh
# Headless Menacer-by-mouse test: boots T2, picks "1 PLAYER - USE MENACER", skips the intro, moves
# the mouse to several points and fires, taking screenshots into build/aimtest (PPM + PNG).
# Look at the PNGs: the game's crosshair must sit on the commanded point, and left click fires.
# usage: port/tools/aimtest.sh [path/to/t2port]
set -e
cd "$(dirname "$0")/../.."
BIN=${1:-build/port/t2port}
OUT=build/aimtest
mkdir -p "$OUT"
rm -f "$OUT"/*.ppm "$OUT"/*.png
if ! SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 300 nice -n 19 \
	"$BIN" --script port/scripts/aimtest.txt --outdir "$OUT" --mute --no-vsync > "$OUT/log.txt" 2>&1; then
	cat "$OUT/log.txt"
	exit 1
fi
python3 -I tools/py/ppm2png.py "$OUT"/*.ppm
tail -n 3 "$OUT/log.txt"
ls "$OUT"/*.png
