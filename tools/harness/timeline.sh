#!/bin/bash
# timeline.sh OUTDIR STATE N STEP [BOTCFG...]   bot-play from STATE for N x STEP frames, writing a
# screenshot (sNN.ppm) and an object dump (o.obj) every STEP frames: a quick look at what happens in a
# level. BOTCFG items are "key=value" botcfg settings (e.g. energy=1). Menacer mode (GUN=1).
# Run from the repository root after building build/harness (tools/harness/levels.sh builds it).
out=${1:?}; st=${2:?}; n=${3:?}; step=${4:?}; shift 4
mkdir -p "$out"; cp "$st" "$out/"
{
  echo "load $(basename "$st" .state)"
  echo "peekeach level FF80CC 2"
  for c in "$@"; do echo "botcfg ${c%%=*} ${c#*=}"; done
  for i in $(seq -w 1 "$n"); do echo "bot $step"; echo "objs o"; echo "shot s$i"; done
  echo "save tl_end"
} > "$out/timeline.txt"
rm -f "$out/o.obj" "$out/bot.log" "$out/peek.log"
GUN=1 GUNMODE=lightgun nice -n 10 timeout 1800 build/harness ext/gpgx/genesis_plus_gx_libretro.so rom/baserom.md \
  "$out/timeline.txt" "$out" > "$out/out" 2>&1
tail -1 "$out/out"
