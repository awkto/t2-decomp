#!/bin/sh
# High frame rate test (docs/hifps.md): the machine state with --fps RATE must equal the 60 Hz run
# frame for frame (presentation only); prints how many frames were interpolated / still / held.
#
#   port/tools/hifpstest.sh                          # all states/*.state, 1500 frames, 120 Hz
#   port/tools/hifpstest.sh --states "gun_01 l8_hit" --frames 600 --fps 240 -v
#
# Builds build/port/t2port if missing. Options go to port/tools/hifpstest.py (--help).
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
[ -x build/port/t2port ] || port/tools/build.sh >/dev/null
exec python3 -I port/tools/hifpstest.py "$@"
