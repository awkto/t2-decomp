# What still runs as 68k with `--hle all`

Measured with `T2_HLE_PROFILE=1` (docs/porting.md): 16 runs, 57000 frames, `--hle all`, lockstep input
script. The runs are the six default states, pad_05, 2p_gun_04, gun_03, gun_08, hk_a, sky_b, l7_chase,
misc_t3 (3000 frames each), the title state left alone for 6000 frames (attract loop), and a cold boot
for 9000 frames. Merge with `port/tools/profmerge.py`. Cycles are 68k cycles; "busy" = everything but idle.
Lockstep `--frames 1000` (six defaults) is identical with the variable set.

## Totals

| | % of all frame time | % of busy |
|---|---|---|
| idle (waiting) | 75.9 | - |
| C replacements (charged) | 10.6 | 44.0 |
| original 68k | 13.5 | 56.0 |

- The C share includes 68k that C calls itself (VramWriteWord and the text printers run nested): 3.1% of
  all, 12.9% of busy. Pure C is 7.5% of all, 31% of busy. So 69% of busy work is still 68k instructions.
- Of the 56.0% real 68k, 28.3 points are routines that are already in C but were declined (an interrupt
  falls inside the call, docs/porting.md "Interrupts inside a replaced function") and ran as the original.
  The rest, 27.7 points (6.7% of all), is code with no replacement yet. Rows below are that part.
- Per run, C is 25-55% of busy (title 25%, attract 43%, boot 36%, gun_05/gun_08/pad_05 about 50-55%).
- The profile is mostly in-level frames: one-shot level loading and the menus barely register (the
  attract loop is 92% idle). Their cost is blocking structure, not CPU.

## Idle (not work)

| addr | name | % of all | note |
|---|---|---|---|
| $0078A6 | FramePaceWait loop | 51.9 | `cmp.w ; bgt.s` until vblank_count reaches frame_target; GameLoop pieces stop before it |
| $0086E2 | WaitVBlank | 23.0 | waits for vblank_count to change (menus, fades, loads) |
| $01DC4E | VDP wait | 1.0 | spins on `(1,a6)` bit 3 then `$C00008` HV counter == 0 |

## Already in C but declined (28.3% of busy, 6.7% of all)

Not remaining port work: the original runs when a VBlank/HBlank would land inside the call. This is the
largest single lever, bigger than any group below. Declined / calls (all runs): VdpCmd1CopyWords
21461/45497 (137 Mcyc, 9.5% busy), VdpCmd7HScroll 36311/36653 (87 Mcyc, 6.1%), BuildSpriteTable 4085/41763
(79 Mcyc, 5.5%), GunpowerHud 1603 and EnergyHud 1689 of 41763 (22 and 16 Mcyc), BgStreamLayers 107103/121667
(10 Mcyc), GameLoopBg6 49/5898 (10 Mcyc, long calls), VdpCmd6TileRect 4505/18528, PlayerShoot 27974/42812,
PlayersButtons 21407/43549, MissilesRun 17982/43549, VdpListRun 36808/47748, ScrollRows 1413/40461.

## Remaining 68k, by group

Mcyc = million 68k cycles over the 57000 frames; % is of busy. Difficulty: easy = work RAM / pure;
medium = VDP port I/O or nested calls (the vdplist.c pattern); hard = Z80 bus / blocking waits / large.

### Level loading and VDP uploads (3.4% of busy)

| addr | name | Mcyc | % | calls | what | difficulty |
|---|---|---|---|---|---|---|
| $00800C | VramClearDma | 15.9 | 1.10 | 24 | DMA fill of VRAM; polls DMA busy, ~660k cycles per call | medium: VDP DMA + wait, lump charge |
| $008782 | VramFillWords | 11.4 | 0.79 | 4252 | d1 bytes at VRAM d2 = word d6, one VramWriteWord per word | easy-medium: VDP data port loop |
| $0080FE | CramUpload | 10.0 | 0.69 | 2848 | 64 CRAM words from (a0) | easy: VDP only |
| $00873A, $00874E | (VRAM fill loops) | 3.6, 3.8 | 0.25, 0.26 | 37, 39 | fill with `move.w d2,(a5)` + status poll per word | medium: poll loops |
| $011FA2 | DmaToVram | 3.3 | 0.23 | 193 | 68k to VRAM DMA | medium: DMA timing |
| $0086C4 | RamClear | 1.1 | 0.07 | 1 | clears work RAM at boot | easy |

Level setup proper (LevelLoad*, script loaders) is only a few calls and does not show; levelflow.c covers it.

### Sound engine ($0F8000-$0FC5CC, 8.0% of busy): now in C (game/sound.c)

The 68k side of the driver tick ($0FBBD0 from VBlankInt, every frame, with the tick body $0FBBFE, the
channel update / note fetch / event reader, the sfx tick, the command slots, the Z80 BUSREQ acquire /
release with its BUSACK spin and the wait for the Z80 command byte) is ported as 24 entries of one
generated function, `snd_run()`; every Z80 / YM access goes through hle_pre()/hle_post() in the original
order and the spins iterate one pass at a time. Lockstep is exact with `--z80`. Details and gates:
docs/porting.md. Left as 68k: nothing in the tick path. What stays outside the C: the driver's reset /
Z80 boot code ($0FBB00, $0FC32E, $0FC350, $0FC3E6, $0FC438 and the Z80 blob at $0FC5CC), which runs at
boot only and is not reached by the tick; and the Z80 program itself, which is Z80 code, not 68k.
The sequence data in the $0F8000 bank is read by the C straight from the ROM.

Request side (`SndRequest` $013D04, `SndMusicRequest`, `SndSlotRequest`) is already in C.

### Menus, screens and text (8.8% of busy)

Text and HUD drawing is the cost; the menu/title/story code itself is almost free (OptionsInput, MainMenuInput,
TitleScreen, WaitStartWindow, CheatCodeCheck: under 0.05 Mcyc each) but is wait-driven.

| addr | name | Mcyc | % | calls | what | difficulty |
|---|---|---|---|---|---|---|
| $0087C4 | VramWriteWord | 55.3 (+59.7 nested) | 3.83 | 549426 | VRAM[d2] = d0.w, leaf | easy: VDP port write; best done with its callers |
| $007D0E | (digit string printer) | 47.0 | 3.26 | 75505 | BCD digits of (a0) as tiles at d3, leading-zero blanking | easy-medium: RAM + VramWriteWord |
| $007E08 | (text printer) | 9.0 | 0.62 | 2314 | text with newline/colour codes, tile base `$FF9E2C` | medium |
| $007C9A, $007CD8 | (HUD score printers) | 5.0, 4.9 | 0.35, 0.34 | 43564 | P1/P2 score and lives via $007D0E | easy |
| $007EB2 | (string printer) | 3.1 (+47.6 nested) | 0.21 | 4133 | bytes to tiles until 0 | easy |
| $0042CE | StoryIntro | 0.8 | 0.06 | 3 | intro story screens: WaitVBlank, PalFade, VdpList, sound | hard: 830 bytes, blocking waits |

### Other (7.4% of busy)

| addr | name | Mcyc | % | calls | what | difficulty |
|---|---|---|---|---|---|---|
| $00C48E | (object animation step) | 27.0 | 1.87 | 15046 | a6 = object: countdown +$1A, next frame from list at +$16; called via $00C442 and $013188 | easy: pure RAM |
| $008C8E, $008C52 | (per-player state handler) | 11.8, 3.4 | 0.82, 0.23 | 87127 | a4 = $FF8000/$FF8050 player struct, by state word +$22: timers, sounds, ReadInput, WaitFrames | medium-hard: has blocking waits |
| $012234 | VdpQueueTileRect | 9.0 | 0.62 | 2199 | VdpList command 6 builder (rect of tilemap words) | medium: VdpList writer |
| $012BB0 | PickupsRun | 8.4 | 0.58 | 43549 | pool $FF9AB2 ($1A each), like MissilesRun | easy: pure |
| $011D38.. | VdpListNext / Reset / End / Jump | 5.9, 3.5, 2.4, 0.4 | 0.41 to 0.03 | 40k-179k | 3-instruction list helpers | easy, but per-call gain small |
| $013376 | SpriteQueueReset | 4.2 | 0.29 | 44061 | resets sprite queue | easy |
| $00B3AE | (object loop dispatch) | 4.2 | 0.29 | 623 | `jmp (a0)` into object handlers; structural | n/a (indirect jump; ObjRunAll owns it) |
| $013F7E | (event ring consumer) | 4.1 | 0.29 | 43549 | drains byte-pair ring at $FF80A0 (head $FF80B0, tail $FF80B2); Random, VdpQueueTileRect, QueueSprite | medium: 570 bytes, 3 pieces |

## Suggested order

1. The declines (28% of busy): cheapest big win, no new routines. Look at why VdpCmd7HScroll (99% declined) and
   VdpCmd1CopyWords (47%) run as 68k; a nested/timed session instead of a decline could move up to ~25% of busy to C.
2. Text/HUD printing with VramWriteWord (8.8%): easy, one cluster ($007C9A..$007EB2, $0087C4).
3. Object animation step and PickupsRun (2.5%): pure RAM.
4. (done) Sound engine tick: game/sound.c. The boot-time Z80 loader ($0FBB00 and callees) is the only sound code left.
5. VDP upload helpers (3.4%): medium; mostly load-time.
