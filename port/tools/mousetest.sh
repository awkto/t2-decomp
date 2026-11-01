#!/bin/sh
# Headless mouse tests (issue #5): the whole game with the mouse alone, run in the port's script
# mode with SDL's dummy drivers. Needs build/port/t2port and the ROM; no display, no pad commands.
#   flow      power-on -> logos -> title -> menu -> OPTIONS -> level 1, clicks only (menuflow.txt)
#   twop      "2 PLAYER - 1 MENACER / 2 JOYPAD 1": mouse = P1, scripted pad = P2
#   continue  die, continue by left click, die again, high-score initials by clicks
#   missile   right click fires a missile (a 1-frame tap does not: the Menacer debounce)
#   grid      aim at a grid of points: the shot must land within 2 px (aim_correct on)
#   lag       the crosshair reaches a new target in 1-2 frames with aim=fast, about 8 with aim=game
# Results and screenshots (PPM + PNG, look at them) go to build/mousetest/. Exit status 1 on a failure.
# usage: port/tools/mousetest.sh [path/to/t2port] [test ...]
cd "$(dirname "$0")/../.." || exit 2
BIN=${1:-build/port/t2port}
[ $# -gt 0 ] && shift
TESTS=${*:-flow twop continue missile grid lag}
OUT=build/mousetest
mkdir -p "$OUT"
rc=0
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy

# run SCRIPT [options]: runs it in OUT/NAME with its own peek.log
run() {
	name=$1; script=$2; opts=$3
	mkdir -p "$OUT/$name"
	rm -f "$OUT/$name/peek.log"
	# states from the flow run are shared: copy them in
	for s in title menu l1 cont; do [ -f "$OUT/flow/$s.state" ] && [ "$name" != flow ] && cp "$OUT/flow/$s.state" "$OUT/$name/"; done
	timeout 600 nice -n 19 "$BIN" --script "$script" --outdir "$OUT/$name" --mute --no-vsync ${opts:+--options "$opts"} \
		> "$OUT/$name/log.txt" 2>&1 || { echo "$name: the port failed"; tail -5 "$OUT/$name/log.txt"; rc=1; return 1; }
	ls "$OUT/$name"/*.ppm > /dev/null 2>&1 && python3 -I tools/py/ppm2png.py "$OUT/$name"/*.ppm > /dev/null 2>&1
	return 0
}

for t in $TESTS; do
	echo "== $t"
	case $t in
	flow | twop | twopad | continue | missile)
		# the others start from states the flow run saved; `twopad` is an alias for `twop`
		kind=$t
		[ "$t" = twopad ] && kind=twop
		if [ "$t" != flow ] && [ ! -f "$OUT/flow/l1.state" ]; then
			run flow port/scripts/menuflow.txt || continue
		fi
		run "$t" "port/scripts/$( [ "$t" = flow ] && echo menuflow || echo "$kind").txt" || continue
		python3 -I tools/py/mousecheck.py flow "$OUT/$t/peek.log" "$( [ "$t" = flow ] && echo menuflow || echo "$kind")" || rc=1
		;;
	grid)
		[ -f "$OUT/flow/l1.state" ] || run flow port/scripts/menuflow.txt || continue
		python3 -I tools/py/mousecheck.py gengrid "$OUT/grid.txt"
		run grid "$OUT/grid.txt" || continue
		python3 -I tools/py/mousecheck.py grid "$OUT/grid/peek.log" || rc=1
		;;
	lag)
		[ -f "$OUT/flow/l1.state" ] || run flow port/scripts/menuflow.txt || continue
		python3 -I tools/py/mousecheck.py genlag "$OUT/lag.txt"
		for mode in fast game; do
			echo "-- aim=$mode"
			run "lag_$mode" "$OUT/lag.txt" "aim=$mode" || continue
			python3 -I tools/py/mousecheck.py lag "$OUT/lag_$mode/peek.log" "$mode" || rc=1
		done
		;;
	*) echo "unknown test $t"; rc=2 ;;
	esac
done
[ $rc -eq 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $rc
