#!/bin/bash
# run from the repo root on a VM: bash tools/harness/scripts/misc/run.sh SCRIPT.txt OUTDIR STATE...
# runs one misc harness script (save.txt, save2.txt, save3.txt, objs.txt) with the gun_01/gun_03 states copied into OUTDIR
s=$1; out=$2; shift 2
mkdir -p build "$out"
[ -x build/harness ] || gcc -O2 -I ext/gpgx/libretro/libretro-common/include -o build/harness tools/harness/harness.c -ldl
cp states/gun_01.state states/gun_03.state "$out/"
GUN=1 GUNMODE=lightgun timeout 900 build/harness ext/gpgx/genesis_plus_gx_libretro.so rom/baserom.md tools/harness/scripts/misc/$s "$out" > "$out/out" 2>&1
tail -2 "$out/out"
