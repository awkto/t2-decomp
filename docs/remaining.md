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

- (Measured before textprint.c) The C share includes 68k that C calls itself (VramWriteWord and the text printers ran nested): 3.1% of
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

## Already in C but declined (24.3% of busy, 7.9% of all)

Not remaining port work: the original runs when a VBlank/HBlank would land inside the call. The VDP list
copy-words / hscroll loops no longer decline outright: they bail into the 68k at the next frame-loop
event (docs/porting.md), so the 68k part is only the tail of the loop (counted as "nested 68k inside C").
Numbers in this section are from a smaller sample than the tables above: `port/tools/declines.py` over 9
runs of 3000 frames (the six default states, pad_05, gun_03, 2p_gun_04; 27000 frames), after the bail change.
In that sample busy is C 52.7% / 68k 47.3% (the 16-run measurement above was 44.0% / 56.0% before it).
Declined / calls: BuildSpriteTable 2969/25482 (70 Mcyc, 7.9% busy), VdpCmd1CopyWords 5236/18770 (63 Mcyc,
7.2%, mostly bails), VdpCmd7HScroll 1994/2233 (17 Mcyc, 1.9%, bails), GunpowerHud 902 and EnergyHud 990
of 25482 (13 and 10 Mcyc), BgStreamLayers 86021/95773 (9 Mcyc), PlayerShoot, PlayersButtons, MissilesRun
and PlayerButtons (interrupt window, few Mcyc). Reasons per function: `T2_HLE_DECLINES=1`.

## Remaining 68k, by group

Mcyc = million 68k cycles over the 57000 frames; % is of busy. Difficulty: easy = work RAM / pure;
medium = VDP port I/O or nested calls (the vdplist.c pattern); hard = Z80 bus / blocking waits / large.

### Level loading and VDP uploads (3.4% of busy)

| addr | name | Mcyc | % | calls | what | difficulty |
|---|---|---|---|---|---|---|
| $00800C | VramClearDma | 15.9 | 1.10 | 24 | DMA fill of VRAM; polls DMA busy, ~660k cycles per call | left 68k: see below |
| $011FA2 | DmaToVram | 3.3 | 0.23 | 193 | 68k to VRAM DMA | left 68k: see below |

Done (game/vdpfill.c): CramUpload $0080FE, VramFillWords $008782, the fill loops $00873A and $00874E, RamClear
$0086C4 (the boot clear is before any gate state, checked with cold-boot frame hashes, see porting.md).

VramClearDma and DmaToVram stay 68k because their time is DMA time. VramClearDma starts a 65520-byte fill and spins
on the DMA-busy flag for ~660k cycles, i.e. five frames with five VBlank interrupts and the frame loop's
per-line DMA stepping inside the poll; a C function cannot run the frame loop, and the doc's "completes at once"
shortcut (hle_dma_complete) does not reproduce the status the poll reads or the interrupt phase, so it cannot be
lockstep-exact. DmaToVram is a 68k-to-VRAM DMA whose length (up to a whole tile set) halts the 68k per line while
the frame loop advances the DMA, followed by a busy poll; same reason, and only 0.23% of busy.

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

### Menus, screens and text (8.8% of busy; the printing cluster is now C)

Text and HUD drawing was the cost; the menu/title/story code itself is almost free (OptionsInput, MainMenuInput,
TitleScreen, WaitStartWindow, CheatCodeCheck: under 0.05 Mcyc each) but is wait-driven.

Done in `game/textprint.c` (exact VDP timing like vdplist.c; docs/porting.md): $0087C4 VramWriteWord, $007D0E digit
printer, $007C9A/$007CD8 HUD score/lives printers, $007EB2 string printer, $007E08 text printer. They run as C
whenever no line-start event (VBlank, frame wrap, H-Int, gun, DMA) can fall inside the call; otherwise (`safe_run`
declines) the 68k runs them, same as VdpListRun.

| addr | name | Mcyc | % | calls | what | difficulty |
|---|---|---|---|---|---|---|
| $0042CE | StoryIntro | 0.8 | 0.06 | 3 | intro story screens: WaitVBlank, PalFade, VdpList, sound | hard: 830 bytes, blocking waits |
| $007DEE | PrintText | (player.c) | | 2240 | still nested-calls VramWriteWord through `hle_call68k` (now C); falls into $007E08 | could call the C body directly |

### Other (7.4% of busy)

| addr | name | Mcyc | % | calls | what | difficulty |
|---|---|---|---|---|---|---|
| $00C48E | (object animation step) | 27.0 | 1.87 | 15046 | a6 = object: countdown +$1A, next frame from list at +$16; called via $00C442 and $013188 | DONE in C (game/misc2.c; was: easy: pure RAM) |
| $008C8E, $008C52 | (per-player state handler) | 11.8, 3.4 | 0.82, 0.23 | 87127 | a4 = $FF8000/$FF8050 player struct, by state word +$22: timers, sounds, ReadInput, WaitFrames | DONE in C (game/playerstate.c): pieces between the jsr's (blocking waits, sounds, ReadInput stay the real 68k calls); the PlayerDied tail was already screens.c |
| $012234 | VdpQueueTileRect | 9.0 | 0.62 | 2199 | VdpList command 6 builder (rect of tilemap words) | DONE in C (game/misc2.c; was: medium: VdpList writer) |
| $012BB0 | PickupsRun | 8.4 | 0.58 | 43549 | pool $FF9AB2 ($1A each), like MissilesRun | DONE in C (game/misc2.c; was: easy: pure) |
| $011D38.. | VdpListNext / Reset / End / Jump | 5.9, 3.5, 2.4, 0.4 | 0.41 to 0.03 | 40k-179k | 3-instruction list helpers | easy, but per-call gain small |
| $013376 | SpriteQueueReset | 4.2 | 0.29 | 44061 | resets sprite queue | easy |
| $00B3AE | (object loop dispatch) | 4.2 | 0.29 | 623 | `jmp (a0)` into object handlers; structural | n/a (indirect jump; ObjRunAll owns it) |
| $013F7E | (event ring consumer) | 4.1 | 0.29 | 43549 | drains byte-pair ring at $FF80A0 (head $FF80B0, tail $FF80B2); Random, VdpQueueTileRect, QueueSprite | DONE in C (game/ringevt.c): pieces between the jsr's, with the producer $013E30 and the text rows $0141B8/$0141E6 |

## Suggested order

1. (partly done) The declines: VdpCmd1CopyWords/VdpCmd7HScroll now bail into 68k at the event boundary (24.3% of busy
   still declined, see "Already in C but declined").
2. (done) Text/HUD printing with VramWriteWord: game/textprint.c.
3. (done: ObjPathStep, PickupsRun, VdpQueueTileRect, PlayerState idle path in game/misc2.c)
4. (done) Sound engine tick: game/sound.c. The boot-time Z80 loader ($0FBB00 and callees) is the only sound code left.
5. (done) VDP upload helpers: game/vdpfill.c. VramClearDma and DmaToVram stay 68k (DMA timing).
