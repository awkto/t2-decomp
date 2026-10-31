#!/bin/sh
# Re-measure the host-side X correction table (port/src/aimtab.h, docs/gun.md section 4).
# Needs build/port/t2port and the level-1 state from port/tools/mousetest.sh flow
# (build/mousetest/flow/l1.state). Rebuild the port afterwards to use the new table.
# usage: port/tools/aimcal.sh [path/to/t2port]
cd "$(dirname "$0")/../.." || exit 2
BIN=${1:-build/port/t2port}
OUT=build/aimcal
[ -f build/mousetest/flow/l1.state ] || { echo "run port/tools/mousetest.sh flow first" >&2; exit 2; }
mkdir -p "$OUT"
rm -f "$OUT/peek.log"
cp build/mousetest/flow/l1.state "$OUT/"
python3 -I tools/py/aimtab.py sweep "$OUT/sweep.txt"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy nice -n 19 "$BIN" --script "$OUT/sweep.txt" --outdir "$OUT" --mute \
	--options aim_correct=off,aim=fast > "$OUT/log.txt" 2>&1 || { tail "$OUT/log.txt"; exit 1; }
python3 -I tools/py/aimtab.py build "$OUT/peek.log" port/src/aimtab.h
