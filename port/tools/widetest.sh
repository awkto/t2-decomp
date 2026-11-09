#!/bin/sh
# Widescreen tests (issue #13, docs/widescreen.md). Headless, needs build/port/t2port, the ROM and states/.
#   mouse   the pointer over the wide frame: the crosshair lands where the pointer is inside the game's
#           320 pixels, waits at the edge over a margin, and a click over a margin fires nothing
#   state   the game state with widescreen on equals the state with it off, frame by frame (3000 frames
#           of the lockstep input from six states; the machine hashes are those of lockstep.sh)
#   shots   screenshots of the feature on, runs/widetest/*.png
#   stats   the renderer's own counters (4:3 part of every wide line equals the 4:3 line, stale margins)
# usage: port/tools/widetest.sh [test ...]      (default: mouse state shots stats)
cd "$(dirname "$0")/../.." || exit 2
BIN=${BIN:-build/port/t2port}
TESTS=${*:-mouse state shots stats}
OUT=runs/widetest
mkdir -p "$OUT"
rc=0
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy T2_NATIVE=1
run() { nice -n 19 timeout -s KILL 600 "$BIN" --hle all --mute --no-vsync "$@"; }

for t in $TESTS; do
	echo "== $t"
	case $t in
	mouse)
		rm -rf "$OUT/mouse"
		run --widescreen 16:9 --script port/scripts/wide/mouse.txt --outdir "$OUT/mouse" > "$OUT/mouse.log" 2>&1
		python3 -I port/tools/widetest.py mouse "$OUT/mouse/peek.log" || rc=1
		;;
	state)
		python3 -I port/tools/widetest.py state --bin "$BIN" || rc=1
		;;
	shots)
		python3 -I port/tools/widetest.py shots "$OUT" --bin "$BIN" || rc=1
		;;
	stats)
		for s in gun_01 gun_02 gun_03 gun_04 gun_05 gun_06 gun_07 gun_08 title; do
			printf 'load states/%s.state\nrun 3000\n' "$s" > "$OUT/stats_$s.txt"
			echo "$s: $(T2_WIDE_STATS=1 run --widescreen 16:9 --script "$OUT/stats_$s.txt" --outdir "$OUT/stats" 2>&1 | grep '^wide:' | tail -1)"
		done
		;;
	*)
		echo "unknown test $t"
		rc=1
		;;
	esac
done
[ $rc = 0 ] && echo "widetest: PASS" || echo "widetest: FAIL"
exit $rc
