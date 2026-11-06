#!/bin/bash
# on a VM, repo root: damage-path difftest and seqcmp of the truck states (needs misc_t4/t5 in states/)
port/tools/build.sh > /dev/null 2>&1
cmake --build build/port -j2 --target difftest > /dev/null 2>&1
build/port/difftest capture 00D112 200 --script port/scripts/capture_misc_t.txt --out runs/states/00D112_dmg 2>&1 | tail -2
build/port/difftest run TruckMain --states runs/states/00D112_dmg 2>&1 | tail -2
build/port/difftest run EndoHitDown3 --states runs/states/00B796 --entry 2>&1 | tail -1
python3 port/tools/seqcmp.py EndoHitDown3 --state misc_q2 --frames 600 | tail -2
for p in "TruckMain misc_t4" "TruckMain misc_t5" "TruckDebris misc_t4" "TruckDie misc_t4" "TruckDebris misc_t5"; do set -- $p; python3 port/tools/seqcmp.py $1 --state $2 --frames 600 | tail -3; done
