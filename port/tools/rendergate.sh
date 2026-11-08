#!/bin/sh
# Render gate (docs/render.md): the C renderer must draw every frame pixel-identical to GPGX, over
# all savestates in states/ (3000 frames of the lockstep input) and the five route scripts.
#
#   port/tools/rendergate.sh                       # everything, --hle all, 4 parallel runs
#   port/tools/rendergate.sh --states "gun_01 title" --routes none --frames 600
#
# Builds build/port/t2port if missing. Options go to port/tools/rendergate.py (--help).
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
[ -x build/port/t2port ] || port/tools/build.sh >/dev/null
exec python3 -I port/tools/rendergate.py "$@"
