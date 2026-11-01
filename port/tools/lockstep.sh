#!/bin/sh
# Lockstep gate (docs/porting.md, "The lockstep gate"): run --hle none and --hle SPEC from
# the same savestate with the same scripted input for N frames and compare the game's work
# RAM and VRAM/CRAM/VSRAM/VDP registers every frame.  Reports the first differing frame and,
# with --addr, the differing addresses; or "<N> frames identical".
#
#   port/tools/lockstep.sh                          # --hle all from the six default states
#   port/tools/lockstep.sh --hle PalFade            # one function, 3000 frames
#   port/tools/lockstep.sh --hle Random --frames 600 --addr
#   port/tools/lockstep.sh --states "gun_01 title" --frames 3000
#
# Builds build/port/t2port if missing.  All options go to port/tools/lockstep.py (--help).
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
[ -x build/port/t2port ] || port/tools/build.sh >/dev/null
exec python3 -I port/tools/lockstep.py "$@"
