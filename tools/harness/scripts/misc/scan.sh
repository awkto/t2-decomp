#!/bin/bash
# run from the repo root on a VM: bash tools/harness/scripts/misc/scan.sh LEVEL(01|03) [OUTDIR]
# prints "entry count first-frame" for every state entry in scanLL.txt
l=${1:-01}; out=${2:-runs/misccov$l}
mkdir -p build "$out"
[ -x build/harness ] || gcc -O2 -I ext/gpgx/libretro/libretro-common/include -o build/harness tools/harness/harness.c -ldl
cp states/gun_$l.state "$out/"
GUN=1 GUNMODE=lightgun timeout 1200 build/harness ext/gpgx/genesis_plus_gx_libretro.so rom/baserom.md tools/harness/scripts/misc/scan$l.txt "$out" > "$out/out" 2>&1
tail -3 "$out/out"
awk '{c[$2]++; if(!($2 in f))f[$2]=$1} END{for(k in c) print k, c[k], f[k]}' "$out/bp.log" | sort
