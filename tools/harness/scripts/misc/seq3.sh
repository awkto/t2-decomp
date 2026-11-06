#!/bin/bash
# on a VM, repo root: seqcmp of the rare ai_misc.c states from the poked misc_q* states
for p in "EndoDieDebris misc_q1" "EndoHitDown3 misc_q2" "Type0DDieDebris misc_q3" "Type0DGone misc_q3"; do set -- $p; echo "== $1 $2"; python3 port/tools/seqcmp.py $1 --state $2 --frames 600 2>&1 | tail -4; done
