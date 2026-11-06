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

## Level-4 objects (panels, orbs, movers), made by the gun-bot from gun_04 (tools/harness/scripts/l4/)
`cand.txt` plays gun_04 with the energy cheat and saves/dumps at chosen frames; `gen.txt` renames them and makes the `_p` copies.
- `l4_lamp`, `l4_box_a`, `l4_box_b`: ceiling lamps about to be shot / supply box (flash, kill states $DAEE, $DB0C, $DBC4).
- `l4_mv_a`, `l4_mv_b`: $24 background movers (kill states $E174, $E1CA).
- `l4_pan_a`, `l4_pan_b`: $22 wall panels with the controllers; `l4_orb_a`, `l4_orb_b`, `l4_orb_c`: $23 orbs (attack, kill $F65A, $E0A4).
- `*_p`: copies with the hp (+8) of the $22/$23/$24 slots poked to 3, so the lockstep input kills them.
Used by port/scripts/capture_l4b.txt and capture_l4c.txt (ai_l4.c difftests).
