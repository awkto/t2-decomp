#!/bin/bash
# Interpreted-instruction profile over every gate route with --hle all (t2-decomp#11, docs/remaining68k.md).
#   port/tools/ipprof.sh [GATE...]     run on a VM after port/tools/build.sh with states/ filled in;
#                                      writes runs/ipprof/*.PID.txt, then aggregate with
#   python3 -I tools/py/remaining68k.py runs/ipprof > docs/remaining68k.md
# Gates (default: all of them):
#   lock    the six default lockstep states, FRAMES (3000) each, the lockstep input script
#   wide    every state in states/ (the 92 wide-gate states), FRAMES each, JOBS (4) in parallel
#   routes  coldboot_gate ringstate_gate pause_gate levelflow_gate screens_gate (port/scripts/)
# STATES="a b" restricts lock/wide, ROUTES="x y" restricts routes.  Each run writes its profile at
# exit (T2_IPPROF, src/ipprof.c).  T2_NATIVE=1 runs the same gates on the native runner (it stops at
# the first "no C block": the log is runs/ipprof/work/NAME.log).
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root" || exit 2
out=${OUT:-runs/ipprof}
mkdir -p "$out/work"
export out
export P=build/port/t2port
export FRAMES=${FRAMES:-3000}
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
gates=${*:-"lock wide routes"}

run() {
	j=$1
	case $j in
	route:*)
		r=${j#route:}
		T2_IPPROF="$out/rt_$r.%d.txt" timeout 1800 $P --hle all --script "port/scripts/$r.txt" \
			--outdir "$out/work/$r" --mute --no-vsync >"$out/work/$r.log" 2>&1 || echo "  $r: exit $?"
		;;
	*)
		python3 -I -c "import sys; sys.path.insert(0,'port/tools'); import lockstep; lockstep.write_script('$out/work/$j.txt','$j',$FRAMES)"
		T2_IPPROF="$out/st_$j.%d.txt" timeout 900 $P --hle all --script "$out/work/$j.txt" \
			--outdir "$out/work/$j" --mute --no-vsync >"$out/work/$j.log" 2>&1 || echo "  $j: exit $?"
		;;
	esac
}
export -f run

: >"$out/work/jobs.txt"
for g in $gates; do
	case $g in
	lock) for s in ${STATES:-gun_01 gun_02 gun_04 gun_05 gun_07 title}; do echo "$s"; done ;;
	wide) for s in ${STATES:-$(ls states/*.state | xargs -n1 basename | sed s/.state//)}; do echo "$s"; done ;;
	routes) for r in ${ROUTES:-coldboot_gate ringstate_gate pause_gate levelflow_gate screens_gate}; do echo "route:$r"; done ;;
	esac
done | sort -u >>"$out/work/jobs.txt"
xargs -P "${JOBS:-4}" -I{} bash -c 'run {}' <"$out/work/jobs.txt"
echo "profiles: $(ls "$out"/*.txt | wc -l) files in $out"
$P --hle all --hle-list 2>/dev/null >"$out/hle-list.txt"
