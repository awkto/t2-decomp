# Level data: scripts, scroll tables, maps (GitLab #7)

The technical format of a level. (The gameplay table of what each level contains is `docs/levels.md`.)
Tool: `tools/py/levelscript.py` decodes every script from the ROM:

    python3 -I tools/py/levelscript.py          # all levels
    python3 -I tools/py/levelscript.py 1 5      # some levels
    python3 -I tools/py/levelscript.py --stats  # opcode use per level

A level is three independent data sets, all in ROM:

1. a **script** (timed command list): enemy waves, graphics loads, texts, palette changes, the end;
2. **scroll tables** (cell-row speeds, `vdp.md`), selected by the script;
3. **background maps** (one byte per column block and layer, `vdp.md`), read by the per-level
   streamer as the rows scroll. A map's 0 byte ends the level.

Plus the per-level load routine (`$001D3A` → `$002980` for level 1 etc.: tiles, plane setup,
palettes) and the per-level code inside spawn/AI routines.

## Where

`LevelScripts` `$00B36C`: 12 longs indexed by `level` (0 and 9 unused, `$0A` = hi-score initials):

| level | script |
| --- | --- |
| 1 | `$01E504` |
| 2 | `$020054` |
| 3 | `$01F3B2` |
| 4 | `$01F80E` |
| 5 | `$0205E2` |
| 6 | `$020938` |
| 7 | `$020C1A` |
| 8 | `$020D42` |
| `$0A` | `$020E76` |

The scripts sit together in `$01E504..$020EFF`, outside the code area.

## The script interpreter

RAM: `script_ptr` `$FF80CE`, `script_clock` `$FF80D2`, `script_due` `$FF80D4`, `script_ret`
`$FF9F62`.

- `ScriptStart` `$009326` (level start): `script_clock = 0`, pointer = `LevelScripts[level]`,
  then `ScriptNext`.
- A command is `delay.w opcode.w args...`. `ScriptNext` `$00933E` reads the delay: 0 runs the
  command now, otherwise `script_due = script_clock + delay` and the pointer is saved.
- `ScriptTick` `$009358` (every game frame, `mainloop.md`): `script_clock++`; when it equals
  `script_due`, `ScriptExec` `$009376` reads the opcode and calls `ScriptOps[op]` (`$00938A`, 63
  longs, entry 0 unused) with a0 on the arguments. The handler must leave a0 after them. Then
  `ScriptNext` again, so commands with delay 0 run in the same frame.
- The clock counts game frames, so pause and lag stretch it with the game. Checked: from the `l1`
  state (clock `$06DA` = 1754) the pointer changes exactly at the clock values the decoder prints
  (1788, 1793, 1843, 1923, ...).
- Control flow: `goto` (`$03`), one-level `gosub`/`return` (`$3D`/`$3E`, return address in
  `script_ret`), and the native routine `ScrDemoSkip` `$00B35A` (offset word, taken only in the
  attract demo, so the demo skips the story text). Levels end with `end_level` (`$23`) or loop with
  `goto` until the map or a boss sets `level_exit`.
- `native` (`$14`, `ScrNative` `$00A140`): `jmp` to a ROM routine given as a long. 46 such targets
  are used (one-off spawns, boss setup, palette loads, player setup). Three read their own
  arguments: `ScrLoadPalette` `$00A2A8` (line.w, palette.l), `ScrDemoSkip` `$00B35A` (offset.w),
  `$00A82C` and `$00ACB0` (one word). They are listed in `NATIVE_ARGS` in the tool.

## Opcodes

Argument letters: `w` word, `x`/`y` word coordinates, `l` long; `x?` = X, then a word, then one more
word only if that word is 0. Arg lengths were derived statically (every `(a0)+` on every path of
each handler) and confirmed by decoding all nine scripts to their end without losing sync.
`spawn_tNN` spawns an object of type `$NN` (`objects.md`); a/b/c are variants (other AI/hit
routines or positions). Unnamed `op_NN` are level-specific spawns or effects not yet looked at.

| op | handler | name | args |
| --- | --- | --- | --- |
| `$01` | `$009486` | spawn_t01a | xy |
| `$02` | `$0094F2` | spawn_t01b | x, y (y = 0: Y 0, AI `$00F83E`, +$28 = 160, presumably drops in; y < 0: Y = -y and flag b12) |
| `$03` | `$0099AA` | goto | l |
| `$04` | `$0099AE` | nop | - |
| `$05` | `$0099B0` | scroll_tab_4a88 (`scroll_script = $004A88`) | - |
| `$06` | `$0099BC` | scroll_tab_4a94 | - |
| `$07` | `$009802` | spawn_t07a | - |
| `$08` | `$0097D0` | spawn_t07b | - |
| `$09` | `$0099FE` | spawn_t09 | ww |
| `$0A` | `$009936` | spawn_t0aa | - |
| `$0B` | `$009910` | spawn_t0ab | - |
| `$0C` | `$0098AE` | spawn_t0c | - |
| `$0D` | `$00988E` | spawn_t0d | - |
| `$0E` | `$009B30` | spawn_t0e | w |
| `$0F`-`$11` | `$009ACC`.. | queue_gfx (fixed tile copies through the VDP list) | - |
| `$12` | `$009B78` | spawn_t0fa (crate) | x? |
| `$13` | `$009BD2` | spawn_t0fb (crate) | xw |
| `$14` | `$00A140` | native | l (+ the routine's own args) |
| `$15` | `$009646` | spawn_15 | w |
| `$16` | `$009C18` | text (`$007DEE`, text record) | l |
| `$17` | `$009C2A` | draw_17 (tile rectangles, `$00BE42`) | - |
| `$18` | `$00A138` | poke_long | value.l address.l |
| `$19` | `$00A144` | spawn_t1b | - |
| `$1A` | `$00A228` | op_1a | - |
| `$1B` | `$009F24` | spawn_t17 | - |
| `$1C` | `$00A10A` | op_1c | - |
| `$1D` | `$009576` | spawn_t01c | xy |
| `$1E` | `$0095DE` | spawn_t11 | xy |
| `$1F` | `$009718` | spawn_t28 | xy |
| `$20` | `$0099D4` | spawn_t1a | - |
| `$21` | `$00A130` | poke_word | value.w address.l |
| `$22` | `$009C30` | text_clear (fills the text rows) | - |
| `$23` | `$009C36` | end_level (`level_exit = $FF`) | - |
| `$24` | `$0096B6` | spawn_t1d | xy |
| `$25` | `$009CB2` | load_tiles | mode.w, tile-base word address.l, graphics.l |
| `$26`/`$27` | `$009D22`/`$009E22` | spawn_t13a/b (humans) | - |
| `$28`/`$29` | `$009EA2`/`$009EB2` | op_28/29 (tile rows) | - |
| `$2A`/`$2B` | `$009C4A`/`$009C62` | spawn_t15a/b | - |
| `$2C` | `$00A5F6` | spawn_t23 | ww |
| `$2D` | `$00A644` | op_2d | - |
| `$2E`/`$2F` | `$00A666`/`$00A692` | op_2e/2f | w |
| `$30` | `$00A7E8` | op_30 | ww |
| `$31` | `$00A80C` | op_31 | w |
| `$32`/`$33` | `$00A8CC`/`$00A8F0` | op_32/33 | ww |
| `$34` | `$00AA3E` | op_34 | w |
| `$35`-`$3A` | `$00AA6A`.. | op_35..op_3a (level 2/5/6 spawns at X,Y) | ww |
| `$3B`/`$3C` | `$00AD14`/`$00ACF2` | op_3b/3c | - |
| `$3D` | `$00B2E6` | gosub | l |
| `$3E` | `$00B2F2` | return | - |

`load_tiles` (`$25`): mode 0 copies the whole block (`len.w` + tiles at the graphics address) to
the VRAM tile index held in the given RAM word; 1/-1 copy halves, -2..-5 quarters (the big level-1
blocks are spread over several frames this way). It queues list command 1, so it costs VBlank time
in the next frame. The RAM words (`$FF9E1C..$FF9E70`) are the tile bases that `SpriteMappings`
pieces add (`sprite.md`).

## Example: start of level 1

    01e504  +0    t=0     poke_word        14 $ffff9eb8      sprite_prio_depth = 14
    01e50e  +0    t=0     scroll_tab_4a88
    01e512  +0    t=0     load_tiles       0 $ffff9e3a $0269e0
    01e520  +0    t=0     load_tiles       -2 $ffff9e36 $0bcbba   (and -3, -4, -5: four quarters)
    01e558  +0    t=0     native           $00b35a 168       demo: skip 168 bytes (the story)
    01e562  +0    t=0     text             $002ce6
    01e56a  +130  t=130   text_clear
    ...
    01e586  +0    t=910   native           $00b2fa           ScrSetPlayers
    01e58e  +1    t=911   spawn_t0e        40
    01e5be  +20   t=1023  spawn_t28        110 136
    01e5c6  +70   t=1093  load_tiles       0 $ffff9e3c $027202
    01e5d4  +70   t=1163  spawn_t28        200 136

## Scroll tables and maps

Format and use: `vdp.md` ("Rows" and "Map columns"). Scroll tables are word lists
`rows speed ... 0 rows speed ... -1` at `$004A88`, `$004A94`, `$004C90`, `$004CB8` (and others
selected with `poke_long $FF9E98`). The map byte arrays and their block tables are referenced from
the per-level streamers `BgStreamL1/L2/L4/L5/L6`; decoding them into pictures is future work.
