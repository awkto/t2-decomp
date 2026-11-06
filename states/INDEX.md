# Savestates (not committed)

GPGX libretro savestates for the headless harness (`tools/harness/harness.c`, `load NAME` from the
run's output directory) made from your own ROM. The `.state` files are gitignored; regenerate them on a
VM (or any machine that may run a few minutes of emulation):

    tools/harness/levels.sh runs/lv boot chain ending states

`boot` runs `tools/harness/scripts/levels/boot_*.txt` (power-on, level-skip cheat code at PRESS START,
menu, level 1), `chain` runs `chain_skip_*.txt` (the gun-bot plays 900 frames of each level, then skips
it with the cheat: pause + B+C, and saves a state at every level change), `ending` runs `ending.txt`,
and `states` copies the results here. Every state was taken when the level variable `$FF80CC` changed,
that is at the start of the level's intro text. All game states have the level-skip cheat
(`cheat_levelskip` `$FFA0A6`) turned on; nothing else is modified. Load a Menacer (`gun_*`, `2p_gun_*`)
state with `GUN=1 GUNMODE=lightgun`, a joypad one (`pad_*`, `2p_pad_*`) with `PAD2=1`.

| file | mode (`$FF9DF5`) | content |
|---|---|---|
| `title.state` | - | "PRESS START" title, before the cheat code (pad 1 input window) |
| `gun_01` .. `gun_08` | 4: 1P Menacer | start of level 1..8 |
| `gun_0a` | 4 | high-score initials screen (level `$0A`) after level 8 was skipped |
| `pad_01` .. `pad_08`, `pad_0a` | 5: 1P joypad 1 | same, joypad |
| `2p_gun_01` .. `2p_gun_08`, `2p_gun_0a` | 0: P1 Menacer, P2 joypad 1 | same, two players |
| `2p_pad_01` .. `2p_pad_08`, `2p_pad_0a` | 1: P1 joypad 1, P2 joypad 2 | same, two players |
| `ending_a.state` | 4 | 300 frames into the ending, "Cyberdyne has been destroyed" branch (`$FFA0A8` = 0) |
| `ending_b.state` | 4 | 300 frames into the ending, "research at Cyberdyne will continue" branch |

The endings are forced (`level_exit = 1` poked on level 8, `ending.txt`), because the bot cannot win the
steel mill yet. Level contents and the bot's results: `docs/levels.md`.

## Level-1 boss (HK tank), made by the gun-bot from gun_01 (tools/harness/scripts/hk/)
- `hk_a`: tank spawned (frame ~18903). `hk_d`: after the turret died.
- `hk_p3`, `hk_p5`: copies with the five boss slots' hp poked to 3, so the lockstep input kills the parts (death states).

## Level-8 steel mill (T-1000 / tanker), made by the gun-bot from gun_08 (tools/harness/scripts/l8/save.txt)
- `l8_frz`: frame 1501 with the T-1000 frozen flag `$FF9EC2` set (frozen countdown, gone, meter off follow).
- `l8_shat`: frame 2500, the T-1000's state pointer poked to `$011580` (shatter, fall, get up).
- `l8_hit`: frame 2500, pointer poked to `$011514`, timer 12 (the pose after a hit).
- All three are untracked like the other `.state` files; `cov.sh`/`cov.txt`/`cov2.txt` list which state entries run when.

## Level-4 boss (Skynet CPU), made by the gun-bot from gun_04 (tools/harness/scripts/sky/)
`scan.txt` logs which AI states run when (guns die 11811..12219, intro 12220..12385, parts 12386..16249, finish 16250..17063); `save1.txt` saves the states, `save2.txt` makes the hp-poked copies (`harness` frame counter; the bot has the energy cheat).
- `sky_a`: frame 11620, four wall guns (`$E2E6`) alive. `sky_a_p`: same, guns' hp poked to 0 (the lockstep input kills them: `$E314`, intro).
- `sky_c`: `sky_a` with the four guns already shot (b15 cleared, state pointer poked to `$E314`, `save3.txt`): tile pops and the intro start.
- `sky_b`: frame 12221, intro running (`$E3E2` tile frames, `$E546` sweep, `$E5A6` spawner of the 16 parts).
- `sky_d`: frame 12400, boss parts alive (`$E72E`, `$E74A`, `$E746`). `sky_d_p`: parts' hp poked to 3 (`$E7C0` death).
- `sky_e`, `sky_e_p`: frame 16240, the last part alive (hp 8 / poked to 3): death, then `$E864`.
- `sky_f`: frame 16490, finish (`$E864` explosions, `$E8C8`, `$E91E` countdown with the fade, `$E9C4`).
