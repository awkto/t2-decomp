#!/bin/bash
# run from the repo root on a VM: bash tools/harness/scripts/l7/cov.sh OUTDIR
out=${1:-runs/l7cov}
mkdir -p build "$out"
gcc -O2 -I ext/gpgx/libretro/libretro-common/include -o build/harness tools/harness/harness.c -ldl
cp states/gun_07.state "$out/"
GUN=1 GUNMODE=lightgun timeout 900 build/harness ext/gpgx/genesis_plus_gx_libretro.so rom/baserom.md tools/harness/scripts/l7/${2:-cov.txt} "$out" > "$out/out" 2>&1
tail -3 "$out/out"
awk '{c[$2]++; if(!($2 in f))f[$2]=$1} END{for(k in c) print k, c[k], f[k]}' "$out/bp.log" | sort
