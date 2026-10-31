# Boot, main loop and frame timing (GitLab #7)

How the game gets from power-on to gameplay and how one gameplay frame is put together. Names are in
`data/symbols.txt` and `data/ram.txt` (sections "# --- engine (issue #7)"). Input and the Menacer are
in `docs/gun.md`.

## The short version

- **One frame-locked loop.** `GameLoop` `$00765E` runs the whole game logic once, then waits for the
  next VBlank (`FramePaceWait` `$0078A6`), then runs the VDP command list that the frame built
  (`VdpListRun` `$011D32`). All VDP work happens on the main thread, right after the wait.
- **VBlank does almost nothing**: Menacer buttons (only in Menacer modes), `vblank_count++`, and one
  sound driver tick (`$0FBBD0`). No VDP access, no input, no timers. ExtInt only latches the gun
  (`docs/gun.md`). HBlank is not used (vector 28 = `IntNull`, reg 0 = `$04`).
- **Lag: slowdown, never frame skipping.** If a frame overruns, the next one starts at once, without
  catching up the lost VBlank. Logic and VDP upload stay 1:1, so a heavy scene runs slower. Measured
  on level 1 (1800 frames from the `l1` state, `bp $00765E`): 1799 loop passes, three frames with no
  pass (each followed by a frame with two).
- **Levels are driven by two clocks**: the level script (`ScriptTick`, frame counter + delay list,
  `levels.md`) spawns enemies and loads graphics, and the background map scroller (`BgStreamLayers`)
  ends the level when the map runs out. Either can set `level_exit`.

## Interrupts

| vector | address | what |
| --- | --- | --- |
| reset | `Reset` `$000200` | standard TMSS/VDP/Z80 init stub, then `jmp Boot $007406` |
| VBlank (6) | `VBlankInt` `$000308` | if `control_mode` bit 0 clear: Menacer handshake on port 2 → `gun_btn_raw`, `gun_latch_cur` → `gun_latch_count`. Always: `vblank_count++`, `jsr $0FBBD0` (sound) |
| ExtInt (2) | `ExtInt` `$00039E` | Menacer TH: first HV latch per frame into `gun_ring` |
| all others | `IntNull` `$000306` | `rte` |

`Boot` sets `sr = $2500` (only VBlank). `ReadPad1` and `MenacerInit` run with `$2700` around the
port access and leave `sr = $2000`, so from the first pad read on all levels are enabled, including
ExtInt for the gun.

`WaitVBlank` `$0086E2` spins until `vblank_count` changes. `WaitFrames` `$0086D8` waits d1+1 frames.
Every blocking screen (logos, menus, text, fades) is a small loop around `WaitVBlank`.

## Power-on to gameplay

1. `Reset` → `Boot` `$007406`: sp = `$FFFF9C`, `CramClear`, `RamClear` (`$FF0000..$FFFF8B`; the
   hi-score table, options and the `ram_magic` long above it survive a soft reset), `ResetCheatsRng`
   (`rng_state = $87654321`), sound init `$0FBB00`, `InitPort1`, `VdpSetRegs(VdpRegTable)`,
   `VsramClear`, `VramClearDma`, `control_mode = 5`, `sr = $2500`, one `WaitVBlank`, PAL flag
   (`$FFA247` from VDP status bit 0), `HiScoreInitOnce` (writes the default table unless
   `ram_magic == "PAUL"`), `jmp ColdStart`.
2. `ColdStart` `$000900`: VDP reset, `control_mode = 0`, credits = 4, the logo/intro screens
   (`$00099E`, `$000CC6`, `$000D76`, `$0042CE`, `$0041E6`), then `TitleScreen`.
3. `TitleScreen` returns d0 = 0 on timeout: `control_mode = 7` (attract demo) and `GameStart`. If
   the demo was interrupted (`control_mode` 8) it goes back to the title, else to the hi-score screen
   `$003F98` and `ColdStart` again. On Start: main menu `$000ED4` (sets `control_mode`), `GameStart`,
   hi-score screen, `ColdStart`.

## GameStart and the level sequence

`GameStart` `$00745C`: Menacer or port-2 pad init by mode, crosshair start positions, scores and
missiles cleared, `players_alive` = 2 or 1. Then `NextLevel` `$0074D8`:

1. `level++`. In the attract demo level 8 wraps to 1. Level 9 is forced to 8. Level `$0A` is the
   hi-score initials screen (it runs in the same loop, `docs/gun.md`).
2. Unless the new level is 6: `LevelResetPlayers` and the per-level stat counters `$FF9E06..$FF9E18`
   are cleared (level 6 continues level 5's counters).
3. `$001D3A`: per-level load (graphics, maps, palettes; `$002980` L1, `$00241E` L2, `$00281E` L3,
   `$002632` L4, `$00229E` L5, ...). `ClearObjectPools` `$008B3C`.
4. Sprite layout defaults (`$FF9F6A`/`$FF9F6C`), players that are not playing get index `$FFFF`.
5. First frame: `SpriteQueueReset`, HUD (`$007C9A`, `$007CD8`, `$008C52`, `GunpowerHud`,
   `EnergyHud`), `VdpListReset`, `ScriptStart` `$009326`, `BuildSpriteTable`, `VdpListEnd`,
   `VdpListRun`, then a blocking palette fade in (`PalFade` with `pal_fading` set), music
   (`$013E1A`).
6. `vblank_count = $8000`, `frame_target = $8001`, and into `GameLoop`.

## One frame (`GameLoop` `$00765E`)

| # | call | what |
| --- | --- | --- |
| 1 | `PalCycle` `$00E9FA` | only if `pal_cycle_on`: rotates 8 bytes of palette line 0 every 8 frames, full CRAM upload (directly, not through the list) |
| 2 | `GameLoopPauseCheck` `$007676` | Start edge of a playing player → pause (below). Skipped on the hi-score screen |
| 3 | sound request | `sfx_request` → `$013D04` unless a sound is still playing |
| 4 | gunpower | P1 and P2 gunpower `+$60`, saturating |
| 5 | HUD | `$007C9A` missiles, `$007CD8` scores, `$008C52`, `GunpowerHud`, `EnergyHud`: direct VRAM writes (`VramWriteWord`), not through the list |
| 6 | `level_exit` set → `LevelEnd` | |
| 7 | `VdpListReset` `$011D12` | start a new VDP command list |
| 8 | `ScriptTick` `$009358` | level script (`levels.md`) |
| 9 | `ScrollRows` `$004752` | per-row horizontal scroll, row deltas for objects, queue hscroll (list cmd 7) |
| 10 | `BgStreamL1/L2/L4/L5/L6` | background map column streaming for levels 1, 2, 4, 5, 6 |
| 11 | `SpriteQueueReset` `$013376` | |
| 12 | `$013F7E` | HUD message box / pickup message (queues sprites and tile rows) |
| 13 | `ReadInputAim`, `PlayersButtons` | input, crosshairs (first sprites queued), firing, shot hit tests (`docs/gun.md`) |
| 14 | `PickupsRun` `$012BB0` | pickup pool |
| 15 | `MissilesRun` `$0123F0` | player missiles and explosions |
| 16 | `EffectsRun` `$01243C` | fixed animations |
| 17 | `ObjRunAll` `$00B398` | the enemy/human object table (`objects.md`) |
| 18 | `SparksRun` `$0126B4`, `DebrisRun` `$012886` | moving animations |
| 19 | `BuildSpriteTable` `$013272` | sort the queue, build the SAT in RAM, queue its copy (`sprite.md`) |
| 20 | `VdpListEnd` `$011D28` | terminate the list |
| 21 | `FramePaceWait` `$0078A6` | wait until `vblank_count >= frame_target` (sign-safe compare: at the `$FFFF→0` wrap it uses one `WaitVBlank`), then `frame_target = vblank_count + 1` |
| 22 | `VdpListRun` `$011D32` | run the list: tilemap columns, hscroll, SAT, palette lines, VSRAM (`vdp.md`) |
| 23 | `level_exit` set → `LevelEnd`, else loop | |

The HUD is drawn with direct VRAM writes during active display (step 5); everything else that
touches the VDP goes through the command list.

The upload in 22 runs after the VBlank wait, so it starts at the top of the VBlank period. If the
frame overran, the wait does not happen and the upload runs mid-display (possible tearing). The CPU
copy of the 80-entry SAT plus the hscroll table fits easily in VBlank; streamed tilemap columns are
the large item.

## Pause

`GameLoopPauseCheck` `$007676` (Start pressed this frame, not held, by a playing player, not while
the player waits to continue): saves `vblank_count` in `saved_vblank`, then in the attract demo sets
`level_exit = $FF`, `control_mode = 8` (demo interrupted). Otherwise: sound pause `$0FBAD2`, "PAUSE"
on the HUD (`$007AFE`), wait for Start release, then loop `WaitVBlank` + `ReadInput` until Start is
pressed again (with the level-skip cheat, B+C there sets `level_exit`). Then the HUD text is removed
(`$002B5C`), sound resumes (`$0FBAE2`) and `vblank_count` is restored, so `frame_target` stays valid.
Nothing else runs while paused: no VDP list, no objects, no palette cycling.

## Level end (`LevelEnd` `$0078D0`)

`level_exit` is set by: the script command `$23` (`ScrEndLevel`), the background map reaching its
0 byte (`BgStreamLayers`), the level-skip cheat, game over (`$FE`), or the demo being interrupted.

- Hi-score screen (level `$0A`): back to the caller.
- Attract demo (`control_mode` 7/8): one last frame (sprites cleared), music stop, return.
- Otherwise: `$008C68` for P1 and P2 (end-of-level player bookkeeping), music `$39`, 150 frames.
  If `players_alive == 0`: game over text `$0015A2`, return. Levels 5 and 7 go straight to
  `NextLevel`. Level 8: `level_exit == 1` plays the ending `$0015EC`; `== 2` goes on. Other levels:
  music `$3A` and the between-levels screen `$003360`, then `NextLevel`.

## RAM

`vblank_count` `$FFA0EE`, `frame_target` `$FFA0AE`, `saved_vblank` `$FFA238`, `level` `$FF80CC`,
`level_exit` `$FFA234`, `control_mode` `$FF9DF5`, `rng_state` `$FFA0F0`, `ram_magic` `$FFFFF6`.
