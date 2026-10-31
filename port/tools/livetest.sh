#!/bin/sh
# Live mouse-mapping test under Xvfb (needs xvfb-run and xdotool): starts t2port with a real
# window, moves the real X pointer over it and checks the debug overlay's GUN coordinates
# via F12 screenshots (build/livetest/). Window scale 2 (640x448) and 3 (960x672), plus a
# window larger than the game image (letterbox) to check the mapping.
# usage: port/tools/livetest.sh [path/to/t2port]
set -e
cd "$(dirname "$0")/../.."
BIN=$(realpath "${1:-build/port/t2port}")
OUT=$(pwd)/build/livetest
mkdir -p "$OUT"
rm -f "$OUT"/*.ppm "$OUT"/*.png
export BIN OUT
timeout 120 xvfb-run -a -s "-screen 0 1280x800x24" sh -c '
	"$BIN" --scale 2 --mute --no-vsync --overlay --outdir "$OUT" --savedir "$OUT" >"$OUT/log.txt" 2>&1 &
	PID=$!
	sleep 3
	WID=$(xdotool search --name "T2" | head -1)
	xdotool windowmove "$WID" 0 0
	sleep 1
	# window pixel (x,y) -> game pixel (x/2, y/2)
	xdotool mousemove 80 120
	sleep 0.5
	xdotool key F12
	sleep 0.5
	xdotool mousemove 600 400 mousedown 1
	sleep 0.5
	xdotool key F12
	sleep 0.5
	xdotool mouseup 1
	xdotool mousemove 5000 5000
	sleep 0.5
	xdotool key F12
	sleep 0.5
	kill $PID
'
python3 -I tools/py/ppm2png.py "$OUT"/*.ppm
ls "$OUT"/*.png
