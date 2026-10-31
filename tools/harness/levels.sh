#!/bin/bash
# levels.sh OUT [PART...]   regenerate the level savestates and the gun-bot runs (docs/levels.md)
#
# Run from the repository root on a machine that may run long jobs (a VM, see tools/vm): it builds
# build/harness, then runs the scripts in tools/harness/scripts/levels/ with up to 4 processes.
# Every run gets its own directory OUT/<name> (bot.log, peek.log, cov.bin, ijump.txt, states).
# PARTs (default: boot chain):
#   boot    power-on to level 1 in the four modes        -> OUT/boot/{l01,p01,m01,q01}.state
#   chain   bot + level-skip cheat through every level   -> OUT/chain_*/<prefix><level>.state
#   honest  bot without cheats from level 1 (1P Menacer and 1P pad), continues while credits last
#   full    each level from its chain_gun state played to the end with the energy, protect and freeze cheats
#           (CHEATED runs, see tools/harness/bot.c), so every boss and the ending are reached
#   menus   OPTIONS menu, continue, game over and high-score entry (menus.txt); P2 joining a 1P game (join.txt)
#   attract power-on and no input for 40000 frames (attract loop and demo)
set -e
out=${1:?usage: levels.sh OUT [boot|chain|honest|full|menus|attract]...}; shift
parts=${*:-boot chain}
S=tools/harness/scripts/levels
mkdir -p build "$out"
gcc -O2 -I ext/gpgx/libretro/libretro-common/include -o build/harness tools/harness/harness.c -ldl
H="build/harness ext/gpgx/genesis_plus_gx_libretro.so rom/baserom.md"
TO=${TIMEOUT:-1800}

# job NAME ENV SCRIPT [STATE...]: copy states into OUT/NAME, run SCRIPT there
job() {
  local name=$1 env=$2 script=$3; shift 3
  mkdir -p "$out/$name"; rm -f "$out/$name"/bot.log "$out/$name"/peek.log
  for s in "$@"; do cp "$s" "$out/$name/"; done
  env $env timeout "$TO" nice -n 10 $H "$script" "$out/$name" > "$out/$name/out" 2>&1 || echo "$name: exit $?" >&2
  echo "$name: $(tail -1 "$out/$name/out")"
}
export -f job; export out H TO

G="GUN=1 GUNMODE=lightgun"
P="PAD2=1"
for part in $parts; do
  case $part in
  boot)
    job boot "$G" $S/boot_gun.txt; job boot "$G" $S/boot_2p_gun.txt
    job boot "$P" $S/boot_pad.txt; job boot "$P" $S/boot_2p_pad.txt ;;
  chain)
    B=$out/boot
    printf '%s\n' "chain_gun|$G|$S/chain_skip_gun.txt|$B/l01.state" \
      "chain_pad|$P|$S/chain_skip_pad.txt|$B/p01.state" \
      "chain_2p_gun|$G|$S/chain_skip_2p_gun.txt|$B/m01.state" \
      "chain_2p_pad|$P|$S/chain_skip_2p_pad.txt|$B/q01.state" |
      xargs -P 4 -I{} bash -c 'IFS="|" read -r n e s st <<< "{}"; job "$n" "$e" "$s" "$st"' ;;
  honest)
    B=$out/boot
    printf '%s\n' "honest_gun|$G|$S/honest_gun.txt|$B/l01.state" "honest_pad|$P|$S/honest_pad.txt|$B/p01.state" |
      xargs -P 2 -I{} bash -c 'IFS="|" read -r n e s st <<< "{}"; job "$n" "$e" "$s" "$st"' ;;
  full)
    C=$out/chain_gun
    for l in 01 02 03 04 05 06 07 08; do
      st=$C/lv$l.state; [ "$l" = 01 ] && st=$out/boot/l01.state
      nx=$(printf '%x' $((10#$l + 1))); [ "$l" = 08 ] && nx=0
      f=$out/full_$l.txt
      printf '# level %s played to its end with the energy cheat (CHEATED)\nload %s\npeekeach level FF80CC 2\nbotcfg energy 1\nbotcfg protect 1\nbotcfg freeze 1\nbotcfg savelevels next\nbot until %s 90000\nshot end\nbot 6000\nshot after\n' \
        "$l" "$(basename "$st" .state)" "$nx" > "$f"
      echo "full_$l|$G|$f|$st"
    done | xargs -P 4 -I{} bash -c 'IFS="|" read -r n e s st <<< "{}"; job "$n" "$e" "$s" "$st"' ;;
  menus)
    job menus "$G" $S/menus.txt "$out/boot/title.state"
    job join "$P" $S/join.txt "$out/boot/p01.state" ;;
  attract)
    printf '# power-on, no input: attract loop and demo\nrun 40000\n' > "$out/attract.txt"
    job attract "$G" "$out/attract.txt" ;;
  *) echo "unknown part $part" >&2; exit 2 ;;
  esac
done
