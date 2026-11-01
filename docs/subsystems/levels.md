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
routines, positions or argument layouts). A spawn's type is the first word of its 18-byte template
(`ObjSpawnTemplate` `$0092DC`: type.w X.w Y.w row.b depth.b AI.l hit.l hp.w); the handler may then
override fields and read script arguments. The names below for the handlers an earlier pass left as
`op_NN` come from reading each handler and its template; the `handler` column is the address in
`ScriptOps` `$00938A`.

| op | handler | name | args | description |
| --- | --- | --- | --- | --- |
| `$01` | `$009486` | spawn_t01a | xy | type `$01` endoskeleton at X,Y; hit `$0059AC` (head box = instant kill) |
| `$02` | `$0094F2` | spawn_t01b | x, y | type `$01`; y = 0: Y 0, AI `$00F83E`, +$28 = 160 (presumably drops in); y < 0: Y = -y and flag b12 |
| `$03` | `$0099AA` | goto | l | jump to the long address |
| `$04` | `$0099AE` | nop | - | no operation |
| `$05` | `$0099B0` | scroll_tab_4a88 | - | `scroll_script = $004A88` |
| `$06` | `$0099BC` | scroll_tab_4a94 | - | `scroll_script = $004A94` |
| `$07` | `$009802` | spawn_t07a | - | type `$07` (flying HK) |
| `$08` | `$0097D0` | spawn_t07b | - | type `$07`, second variant |
| `$09` | `$0099FE` | spawn_t09 | ww | type `$09` at the two words |
| `$0A` | `$009936` | spawn_t0aa | - | type `$0A` (HK aerial) |
| `$0B` | `$009910` | spawn_t0ab | - | type `$0A`, second variant |
| `$0C` | `$0098AE` | spawn_t0c | - | type `$0C` |
| `$0D` | `$00988E` | spawn_t0d | - | type `$0D` |
| `$0E` | `$009B30` | spawn_t0e | w | type `$0E` at X = w*4, Y = `$58` (AI `$00C0AA`, hit `$005CEE`, hp 5); `$FF9E08++` |
| `$0F`-`$11` | `$009ACC`.. | queue_gfx | - | fixed tile copies through the VDP list |
| `$12` | `$009B78` | spawn_t0fa (crate) | x? | type `$0F` crate at X (+ one more word when the next word is 0) |
| `$13` | `$009BD2` | spawn_t0fb (crate) | xw | type `$0F` crate at X, +$2A = w |
| `$14` | `$00A140` | native | l | `jmp` to the long routine; it may read more args (see `NATIVE_ARGS`) |
| `$15` | `$009646` | spawn_15 | w | type `$03` at X, Y = `$150`, random variant (hit `$005AF8`, AI `$00D978`, hp 15); `$FF9E06++` |
| `$16` | `$009C18` | text | l | print a text record (`PrintText` `$007DEE`) |
| `$17` | `$009C2A` | draw_17 | - | tile rectangles (`$00BE42`) |
| `$18` | `$00A138` | poke_long | value.l address.l | write a long to RAM |
| `$19` | `$00A144` | spawn_t1b | - | type `$1B` (the level-3 truck, cannot be hurt by the player) |
| `$1A` | `$00A228` | draw_tiles_e51e | - | 16x11 tile rect from ROM `$0539EE` to plane-A `$E51E` (OR `$E000`), tile base `$FF9E4A` |
| `$1B` | `$009F24` | spawn_t17 | - | the level-1 HK tank: body/turret/arm parts, types `$16`-`$19` (hit `$006126`-`$006208`) |
| `$1C` | `$00A10A` | draw_tiles_e39a | - | 13x13 tile rect from ROM `$04E284` to plane-A `$E39A` (OR `$8000`), tile base `$FF9E48` |
| `$1D` | `$009576` | spawn_t01c | xy | type `$01` at X,Y |
| `$1E` | `$0095DE` | spawn_t11 | xy | type `$11` at X,Y |
| `$1F` | `$009718` | spawn_t28 | xy | **type `$01`** at X,Y (AI `$00C564`, hit `$005BEC`); despite the name it is a type-`$01` endoskeleton |
| `$20` | `$0099D4` | spawn_t1a | - | type `$1A` |
| `$21` | `$00A130` | poke_word | value.w address.l | write a word to RAM |
| `$22` | `$009C30` | text_clear | - | fill the text rows (`$00BF46`) |
| `$23` | `$009C36` | end_level | - | `level_exit = $FF` |
| `$24` | `$0096B6` | spawn_t1d | xy | type `$1D` endoskeleton at X,Y |
| `$25` | `$009CB2` | load_tiles | mode.w, tile-base word address.l, graphics.l | queue a tile-block copy to VRAM |
| `$26`/`$27` | `$009D22`/`$009E22` | spawn_t13a/b (humans) | - | type `$13`/`$14` humans and the human-scene tile rows; `$FF9E0C++` |
| `$28` | `$009EA2` | clear_tiles_e780 | - | remove every type `$13`/`$14` object (`sub_012986`), then zero-fill 8 rows of `$30` words at `$E780` |
| `$29` | `$009EB2` | clear_tiles_e7b0 | - | zero-fill 8 rows of `$20` words at `$E7B0` |
| `$2A` | `$009C4A` | spawn_t15a | - | queues a tile block (ROM `$048CA6`) through the VDP list (`sub_009B14`); the name is historical, this is not a spawn |
| `$2B` | `$009C62` | spawn_t15b | - | type `$15` at X = `$168`, Y = `$A0` (AI `$00CABA`, hit `$0060F4`, hp 40); `$FF9E0A++` |
| `$2C` | `$00A5F6` | spawn_t23 | ww | type `$23` at the two words; `$FF9E10++` |
| `$2D` | `$00A644` | spawn_t0ea | - | type `$0E` at X = `$154`, Y = `$80` (AI `$00C332`, hit `$005CC6`, hp 10); `$FF9E08++` |
| `$2E` | `$00A666` | spawn_t24a | w | type `$24` (AI `$00E166`, hit `$006456`, hp 80); w -> +$A = `$E300` + w; `$FF9E0E++` |
| `$2F` | `$00A692` | spawn_t24b | w | three type `$24` at X, X+`$40`, X+`$60` (AI `$00E166`, hit `$006488`, hp 40); w -> +$A = `$E400`/`$E410` + w; sets `$FF9EDA` and `$FFA23C` = 2; `$FF9E0E += 3` |
| `$30` | `$00A7E8` | spawn_t0fc | ww | type `$0F` (AI `$00EA26`, hit `$006512`, hp 4); w = X, w -> +$A |
| `$31` | `$00A80C` | spawn_t20 | w | type `$20` (AI `$00EA6A`, hit `$006544`, hp 4); w = X |
| `$32` | `$00A8CC` | spawn_t0fd | ww | type `$0F` (AI `$00EB2E`, hit `$006576`, hp 4); w = X, w = Y |
| `$33` | `$00A8F0` | spawn_t0fe | ww | type `$0F` (AI `$00EB62`, hit `$0065A4`, hp 2); w = X, w = Y |
| `$34` | `$00AA3E` | spawn_t29 | w | type `$29` (AI `$00EC58`, hit `$0067F2`, hp 20); w = X, +$24 = `$10`; `$FF9E12++` |
| `$35` | `$00AA6A` | spawn_t26a | ww | type `$26` (AI `$00EDD4`, hit `$00683A`, hp 10); w = X, w = Y, +$28 = `$BB`; `$FF9E12++` |
| `$36` | `$00AAD2` | spawn_t26b | ww | type `$26` (AI `$00EFA4`, hit `$006894`, hp 10); w = X, w = Y (Y < 0: AI `$00F69C`, +$28 = -Y); `$FF9E12++` |
| `$37` | `$00AB8A` | spawn_t26c | ww | type `$26` (AI `$00F18A`, hit `$0068CA`, hp 10); w = X, w = Y (Y < 0: AI `$00F6F4`, +$28 = -Y); `$FF9E12++` |
| `$38` | `$00AC6A` | spawn_t23a | ww | type `$23` (AI `$00F540`, hit `$0067C6`, hp 30); w = X, w -> +$28 (w < 0: +$26 = `$FFFF`, +$28 = -w); `$FF9E10++` |
| `$39` | `$00ADD0` | spawn_t27a | ww | type `$27` (AI `$00FBAA`, hit `$006AE0`, hp 10); w = X, w = Y (Y < 0: AI `$00FE3C`, +$28 = -Y); `$FF9E0A++` |
| `$3A` | `$00AE72` | spawn_t27b | ww | type `$27` (AI `$00FF12`, hit `$006B32`, hp 10); w = X, w = Y (Y < 0: AI `$0100D0`, +$28 = -Y); `$FF9E0A++` |
| `$3B` | `$00AD14` | spawn_t28b | - | type `$28` (AI `$010238`, hit `$006B96`, hp 400); helper `$010408` |
| `$3C` | `$00ACF2` | spawn_t0ff | - | type `$0F` (AI `$010122`, hit `$006B68`, hp 4); `$FF9EDA` = object |
| `$3D` | `$00B2E6` | gosub | l | one-level call, return address in `script_ret` |
| `$3E` | `$00B2F2` | return | - | return from `gosub` |

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
