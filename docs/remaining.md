# What still runs as 68k with `--hle all`

Measured with `T2_HLE_PROFILE=1` (docs/porting.md): 16 runs, 57000 frames, `--hle all`, lockstep input
script. The runs are the six default states, pad_05, 2p_gun_04, gun_03, gun_08, hk_a, sky_b, l7_chase,
misc_t3 (3000 frames each), the title state left alone for 6000 frames (attract loop), and a cold boot
for 9000 frames. Merge with `port/tools/profmerge.py`. Cycles are 68k cycles; "busy" = everything but idle.
Lockstep `--frames 1000` (six defaults) is identical with the variable set.

## Totals

| | % of all frame time | % of busy |
|---|---|---|
| idle (waiting) | 77.7 | - |
| C replacements (charged) | 15.6 | 69.9 |
| original 68k | 6.7 | 30.1 |

- The C share includes 68k that C calls itself (VramWriteWord and the text printers run nested): 1.3% of
  all, 5.8% of busy. Pure C is 14.3% of all, 64% of busy. So 36% of busy work is still 68k instructions
  (real 68k plus the nested part).
- Of the 30.1% real 68k, 25.2 points (5.6% of all) are routines that are already in C but were declined (an
  interrupt falls inside the call, docs/porting.md "Interrupts inside a replaced function") and ran as the
  original. The rest, 5.0 points (1.1% of all), is code with no replacement yet. Rows below are that part.
  (The "real 68k" of a declined routine also contains the C it calls: a declined GunpowerHud runs the
  original `jsr`s into the C StringPrint and the profiler books that C time to the caller, so the declined
  numbers are an upper bound.)
- Per run, C is 50-88% of busy (gun_02 50%, boot 63%, gun_04 65%, title/attract 69-70%, gun_08 82%, pad_05 88%).
- The profile is mostly in-level frames: one-shot level loading and the menus barely register (the
  attract loop is 92% idle). Their cost is blocking structure, not CPU.

## Idle (not work)

| addr | name | % of all | note |
|---|---|---|---|
| $0078A6 | FramePaceWait loop | 51.9 | `cmp.w ; bgt.s` until vblank_count reaches frame_target; GameLoop pieces stop before it |
| $0086E2 | WaitVBlank | 23.0 | waits for vblank_count to change (menus, fades, loads) |
| $01DC4E | VDP wait | 1.0 | spins on `(1,a6)` bit 3 then `$C00008` HV counter == 0 |

## Already in C but declined (25.2% of busy, 5.6% of all)

Not remaining port work: the original runs when a VBlank/HBlank would land inside the call. The VDP list
copy-words / hscroll loops no longer decline outright: they bail into the 68k at the next frame-loop
event (docs/porting.md), so the 68k part is only the tail of the loop (counted as "nested 68k inside C").
Numbers in this section are the same 16-run, 57000-frame measurement as the totals (`T2_HLE_DECLINES=1`
with `T2_HLE_PROFILE=1`, `port/tools/declines.py` summed over the runs).
Declined / calls, with the real 68k Mcyc and % of busy that the declined calls cost:
VdpCmd1CopyWords 8786/33018 (81 Mcyc, 6.3%; the head did not fit before VBLANK-clear/wrap, 8693),
BuildSpriteTable 4085/41763 (79 Mcyc, 6.1%; 3857 interrupt window, 228 frame end), VdpCmd7HScroll 3372/3921
(27 Mcyc, 2.1%), GunpowerHud 1603 and EnergyHud 1689 of 41763 (22 and 16 Mcyc, 1.7% and 1.2%; bound 5500/5400
cycles against a window that is only 1.4% of a frame), BgStreamLayers 107103/121667 (10 Mcyc, 0.8%; bound 76500),
VramFillWords 2827/4250 (9 Mcyc), VdpCmd6TileRect 4491/18491 (7 Mcyc), ScrollRows 1413/40461 (5.6 Mcyc),
then PlayerShoot 27974/42812, PlayersButtons 21407/43549, MissilesRun 17982/43549 and PlayerButtons
11661/42812 (interrupt window of a worst-case bound, 1 to 3 Mcyc each). Reasons per function: `T2_HLE_DECLINES=1`.

## Remaining 68k, by group

Mcyc = million 68k cycles over the 57000 frames; % is of busy. Difficulty: easy = work RAM / pure;
medium = VDP port I/O or nested calls (the vdplist.c pattern); hard = Z80 bus / blocking waits / large.

### Level loading and VDP uploads (1.5% of busy)

| addr | name | Mcyc | % | calls | what | difficulty |
|---|---|---|---|---|---|---|
| $00800C | VramClearDma | 15.9 | 1.22 | 24 | DMA fill of VRAM; polls DMA busy, ~660k cycles per call | left 68k: see below |
| $011FA2 | DmaToVram | 3.3 | 0.25 | 193 | 68k to VRAM DMA | left 68k: see below |

Done (game/vdpfill.c): CramUpload $0080FE, VramFillWords $008782, the fill loops $00873A and $00874E, RamClear
$0086C4 (the boot clear is before any gate state, checked with cold-boot frame hashes, see porting.md).

VramClearDma and DmaToVram stay 68k because their time is DMA time. VramClearDma starts a 65520-byte fill and spins
on the DMA-busy flag for ~660k cycles, i.e. five frames with five VBlank interrupts and the frame loop's
per-line DMA stepping inside the poll; a C function cannot run the frame loop, and the doc's "completes at once"
shortcut (hle_dma_complete) does not reproduce the status the poll reads or the interrupt phase, so it cannot be
lockstep-exact. DmaToVram is a 68k-to-VRAM DMA whose length (up to a whole tile set) halts the 68k per line while
the frame loop advances the DMA, followed by a busy poll; same reason, and only 0.23% of busy.

Level setup proper (LevelLoad*, script loaders) is only a few calls and does not show; levelflow.c covers it.

### Sound engine ($0F8000-$0FC5CC): now in C (game/sound.c)

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

### Menus, screens and text (under 0.5% of busy; the printing cluster is now C)

Text and HUD drawing was the cost; the menu/title/story code itself is almost free (OptionsInput, MainMenuInput,
TitleScreen, WaitStartWindow, CheatCodeCheck: under 0.05 Mcyc each) but is wait-driven.

Done in `game/textprint.c` (exact VDP timing like vdplist.c; docs/porting.md): $0087C4 VramWriteWord, $007D0E digit
printer, $007C9A/$007CD8 HUD score/lives printers, $007EB2 string printer, $007E08 text printer. They run as C
whenever no line-start event (VBlank, frame wrap, H-Int, gun, DMA) can fall inside the call; otherwise (`safe_run`
declines) the 68k runs them, same as VdpListRun.

| addr | name | Mcyc | % | calls | what | difficulty |
|---|---|---|---|---|---|---|
| $0042CE | StoryIntro | <0.45 | <0.04 | 3 | intro story screens: WaitVBlank, PalFade, VdpList, sound | hard: 830 bytes, blocking waits |
| $007DEE | PrintText | (player.c) | | 2240 | still nested-calls VramWriteWord through `hle_call68k` (now C); falls into $007E08 | could call the C body directly |

### Other (2.7% of busy)

The work-RAM leftovers of the earlier list (ObjPathStep $00C48E, PlayerState $008C8E/$008C52, VdpQueueTileRect
$012234, PickupsRun $012BB0) are C now (game/misc2.c) and no longer show.

| addr | name | Mcyc | % | calls | what | difficulty |
|---|---|---|---|---|---|---|
| $00B3AE | (object loop dispatch) | 4.2 | 0.32 | 623 | `jmp (a0)` into object handlers; structural | n/a (indirect jump; ObjRunAll owns it) |
| $013376 | SpriteQueueReset | 4.2 | 0.32 | 44061 | resets sprite queue | easy |
| $011D38 | VdpListNext | 4.2 | 0.32 | 127128 | VdpList helper, 3 instructions | easy, but per-call gain small |
| $013F7E | (event ring consumer) | 4.1 | 0.32 | 43549 | drains byte-pair ring at $FF80A0 (head $FF80B0, tail $FF80B2); Random, VdpQueueTileRect, QueueSprite | medium: 570 bytes, 3 pieces |
| $011D12 | VdpListReset | 3.5 | 0.27 | 47757 | VdpList helper | easy, per-call gain small |
| $011D28 | VdpListEnd | 2.4 | 0.18 | 47757 | VdpList helper | easy, per-call gain small |
| $004916, $0048C4, $004820, $004872 | BgStreamL1, L4, L5, L2 | 2.7, 1.8, 1.3, 0.6 | 0.21 to 0.05 | 11659, 8773, 6336, 2943 | per-layer wrappers that `bsr` into BgStreamLayers (C) | easy; the GameLoopBg* pieces cover the calls from the loop |
| $00DB36 | (object helper) | 1.4 | 0.11 | 11003 | | easy |
| $013188 | (sprite queue helper) | 1.2 | 0.09 | 10052 | calls the object animation step $00C48E | easy |
| $00D238 | (truck helper) | 1.0 | 0.07 | 3714 | | easy |
| $00920C | (HUD bar helper) | 0.9 | 0.07 | 3198 | builds the bar string, then StringPrint (C) | easy |
| $00B3C6, $009356, $00C442 | (object loop, script tick, animation helpers) | 0.7, 0.7, 0.7 | 0.06 to 0.05 | 44059, 43562, 4347 | | easy |

## Suggested order

1. (partly done) The declines: VdpCmd1CopyWords/VdpCmd7HScroll now bail into 68k at the event boundary (24.3% of busy
   still declined, see "Already in C but declined").
2. (done) Text/HUD printing with VramWriteWord: game/textprint.c.
3. (done: ObjPathStep, PickupsRun, VdpQueueTileRect, PlayerState idle path in game/misc2.c)
4. (done) Sound engine tick: game/sound.c. The boot-time Z80 loader ($0FBB00 and callees) is the only sound code left.
5. (done) VDP upload helpers: game/vdpfill.c. VramClearDma and DmaToVram stay 68k (DMA timing).
