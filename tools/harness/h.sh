#!/bin/bash
# h.sh OUTDIR SCRIPT   run the harness (Menacer enabled) on SCRIPT with outputs in OUTDIR; prints the last output line
# Run from the repository root; build the harness first (see README Setup).
out=$1
mkdir -p "$out"
GUN=1 nice -n 19 timeout 900 tools/harness/harness ext/gpgx/genesis_plus_gx_libretro.so rom/baserom.md "$2" "$out" > "$out/out" 2>&1
tail -1 "$out/out"
