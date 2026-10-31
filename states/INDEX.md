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
