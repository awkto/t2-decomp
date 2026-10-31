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
#   full2p / fullpad   the same in 2P mode 0 (Menacer + pad 1) and in 1P joypad mode
#   ending  the ending, forced on level 8 (ending.txt, CHEAT: level_exit = 1)
#   states  copy the level-start states into states/ (states/INDEX.md)
#   menus   OPTIONS menu, continue, game over and high-score entry (menus.txt); P2 joining a 1P game (join.txt)
#   attract power-on and no input for 40000 frames (attract loop and demo)
set -e
out=${1:?usage: levels.sh OUT [boot|chain|honest|full|full2p|fullpad|ending|menus|attract|states]...}; shift
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
  full|full2p|fullpad)
    # name prefix, env, chain dir + state prefix, level-1 state, bot devices
    case $part in
      full)    pre=full;  env=$G; C=$out/chain_gun;    sp=lv; l1=$out/boot/l01.state; dev="p1 gun" ;;
      full2p)  pre=f2p;   env=$G; C=$out/chain_2p_gun; sp=m;  l1=$out/boot/m01.state; dev="p1 gun,p2 pad1" ;;
      fullpad) pre=fpad;  env=$P; C=$out/chain_pad;    sp=p;  l1=$out/boot/p01.state; dev="p1 pad1" ;;
    esac
    for l in 01 02 03 04 05 06 07 08; do
      st=$C/$sp$l.state; [ "$l" = 01 ] && st=$l1
      nx=$(printf '%x' $((10#$l + 1))); [ "$l" = 08 ] && nx=0
      f=$out/${pre}_$l.txt
      { printf '# level %s played to its end with the energy, protect and freeze cheats (CHEATED)\n' "$l"
        printf 'load %s\npeekeach level FF80CC 2\n' "$(basename "$st" .state)"
        echo "$dev" | tr ',' '\n' | sed 's/^/botcfg /'
        printf 'botcfg energy 1\nbotcfg protect 1\nbotcfg freeze 1\nbotcfg savelevels next\n'
        printf 'bot until %s 90000\nshot end\nbot 6000\nshot after\n' "$nx"; } > "$f"
      echo "${pre}_$l|$env|$f|$st"
    done | xargs -P 4 -I{} bash -c 'IFS="|" read -r n e s st <<< "{}"; job "$n" "$e" "$s" "$st"' ;;
  ending)
    job ending "$G" $S/ending.txt "$out/chain_gun/lv08.state" ;;
  menus)
    job menus "$G" $S/menus.txt "$out/boot/title.state"
    job join "$P" $S/join.txt "$out/boot/p01.state" ;;
  states)
    # collect the level-start states into states/ (see states/INDEX.md); needs boot, chain, ending
    mkdir -p states
    cp "$out/boot/title.state" states/title.state
    for m in gun:l pad:p 2p_gun:m 2p_pad:q; do
      d=${m%%:*}; p=${m#*:}; c=$out/chain_$d; [ "$p" = l ] && c=$out/chain_gun
      cp "$out/boot/${p}01.state" "states/${d}_01.state"
      for l in 02 03 04 05 06 07 08 0a; do
        f=$c/$p$l.state; [ "$p" = l ] && f=$c/lv$l.state
        [ -f "$f" ] && cp "$f" "states/${d}_$l.state"
      done
    done
    for e in ending_a ending_b; do [ -f "$out/ending/$e.state" ] && cp "$out/ending/$e.state" states/; done
    ls states ;;
  attract)
    printf '# power-on, no input: attract loop and demo\nrun 40000\n' > "$out/attract.txt"
    job attract "$G" "$out/attract.txt" ;;
  *) echo "unknown part $part" >&2; exit 2 ;;
  esac
done
