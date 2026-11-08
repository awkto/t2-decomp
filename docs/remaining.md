# What still runs as 68k with `--hle all`

Measured with `T2_HLE_PROFILE=1` (docs/porting.md): 16 runs, 57000 frames, `--hle all`, lockstep input
script. The runs are the six default states, pad_05, 2p_gun_04, gun_03, gun_08, hk_a, sky_b, l7_chase,
misc_t3 (3000 frames each), the title state left alone for 6000 frames (attract loop), and a cold boot
for 9000 frames. Merge with `port/tools/profmerge.py`. Cycles are 68k cycles; "busy" = everything but idle.
Lockstep `--frames 1000` (six defaults) is identical with the variable set.

## Totals

Re-measured on the same 16 runs after helpers.c, vdphelp.c, pools.c, rtsstubs.c, ringevt.c and
playerstate.c (main 228d1c0). Previous column: the measurement before those (docs/porting.md "Early bail and resume").
The tables further down are the new measurement.

| | % of all frame time | % of busy | previous: all / busy |
|---|---|---|---|
| idle (waiting) | 77.3 | - | 78.9 / - |
| C replacements (charged) | 19.9 | 87.4 | 18.0 / 85.0 |
| original 68k | 2.9 | 12.6 | 3.2 / 15.0 |

- The C share includes 68k that C calls itself (VramWriteWord and the text printers run nested): 1.4% of
  all, 6.0% of busy. Pure C is 18.5% of all, 81.4% of busy. So 18.6% of busy work is still 68k instructions
  (real 68k plus the nested part).
- Of the 12.6% real 68k, 10.3 points (2.3% of all) are routines that are already in C but were declined (an
  interrupt falls inside the call, docs/porting.md "Interrupts inside a replaced function") or whose C charge is
  burned at a trap address (see below) and ran as the original. The rest, 2.3 points (0.5% of all), is code with
  no replacement yet. Rows below are that part.
  The "real 68k" of an HLE function is an upper bound: a C call that charges more than one stall slice is burned
  at a trap address and the profiler books those cycles to the function (GunpowerHud and EnergyHud, 24.0 and
  17.8 Mcyc, GameLoopBg6 11.2, GameLoopGunpower 4.3 are their C charge, not 68k work: GunpowerHud/EnergyHud
  decline 187 and 210 times in 44919 calls).
- Per run, C is 68-93% of busy (boot 68%, misc_t3 81%, title 83-87%, gun_01 86%, gun_05 93%, pad_05 91%).
- The profile is mostly in-level frames: one-shot level loading and the menus barely register (the
  attract loop is 92% idle). Their cost is blocking structure, not CPU.
- How it was run: 16 `t2port --hle all --no-vsync --mute` runs with `T2_HLE_PROFILE=1 T2_HLE_DECLINES=1`, the
  lockstep input script (`lockstep.write_script`) from each state; the title run twice (3000 and 6000 frames), the
  cold boot as the script `run 9000`; merged with `port/tools/profmerge.py` over the 16 stderr logs.

## w4_2: no declines, no bails, no nested 68k left in the coroutines

`--hle all` over the 92 gate states (`port/tools/declines.py --frames 1500 <all 92 states>`, `T2_HLE_DECLINES=1`
`T2_HLE_PROFILE=1`, the lockstep input script; the script now also sums the `bails:` and `nested68k:` lines)
**no longer hands any call to the 68k**: 0 declines, 0 bails, no `nested68k:` line. Main 16257d8 against this branch:

| | main 16257d8 | now |
|---|---|---|
| declines (all functions) | 93437 | 0 |
| ... VDP-wait / boot blocks (`bt_Auto_01DC56`, `01DC5C`, `bt_Boot_00025C`, `bt_Boot_0FC3B6`, other `bt_*`) | 92652 (frame end) | 0 |
| ... ReadInputAim / ReadPad1 / BgStreamLayers / GunpowerHud / MissilesRun / MissilesSlot | 324 / 44 / 203 / 42 / 35 / 30 | 0 |
| ... glue blocks (`glue_*`, one instruction each) | 107 | 0 |
| bails | MissilesRun 2958, MissilesSlot 2197, GunpowerHud 646, BgStreamLayers 152 | 0 |
| nested 68k in a coroutine (`nested68k:`) | $009066 (68) | none |
| real 68k, busy time (profile, 92 states x 1500 frames) | 6.82% | 1.10% (VramClearDma 0.84%, the rest PalFade/CramUpload/sub_010666 called from non-coroutine C) |
| gates | wide 92/92, five route gates | wide 92/92 (4 x OK), coldboot 9000, ringstate 53768, pause 5738, levelflow 43913, screens 34882 frames identical; lockstep + `--z80` six states; mousetest ALL PASS |

What changed, function by function:

| function | was | now |
|---|---|---|
| ReadPad1 $0088A0, ReadPad2 $008908 | `HLE_FUNC`, declined (irqwin 44) | `HLE_FUNC_LOWMASK_YIELD`: SR is `$2700` from the first instruction on and `$2000` after the last move to SR (where `hle_irq_poll` yields when an interrupt is pending); effects before charges |
| ReadInput $0003DE, ReadInputAim $0004B6 | `LOWMASK`, batch-charged `HLE_SEQ` groups, declined (irqwin 324) | `LOWMASK_YIELD`; every memory access first in its group, split into one charge per instruction |
| MenacerReadXY $01E442 | `PURE`, charged the prologue and the ring reads in batches | `YIELD`; the ring is read by the instruction that reads it (the gun interrupt writes it), the stores come before their charges |
| BgStreamLayers $00498A | `BAIL` (bailed 152, declined 203) | `YIELD`, bail logic removed; the `add.w d1,(a6)` read-modify-write and the two stores are split |
| MissilesRun $0123F0, MissilesSlot $012402 | `BAIL` (bailed 5155, declined 65) | `YIELD`, `MSL_BAIL` removed (the body was already exact) |
| GunpowerHud $009186 | `BAIL` (646 bails) | `BAIL_YIELD`; the old "one instruction late in 2p_gun_06" was the missing boundary after a callee's `rts` (fixed in `hle_callc`) |
| TruckMain $00D112, BgStructFree $00BE2E | `HLE_RANGE` (declined, nested 68k inside) | `HLE_RANGE_YIELD` |
| TruckDamage $00D322 | `#if 0` (generated pieces; "diverged in misc_t3") | on: the divergence was the nested 68k of sub_00D46E, which spawns 16 objects (96000 mclk) inside the coroutine |
| sub_00BE42, sub_00D444, sub_00D46E, sub_009066, sub_0099C8, sub_00636E, sub_0122CA, sub_0072EA, sub_010D0C | census ranges with a stack delta, so `hle_callc` ran them as nested 68k | whole-function `YIELD` ports (callees4.c, callees5.c, `piecegen.py funcs`) |
| the $00D444 nested call of TruckDamage | nested 68k | direct C call (above) |
| `glue_*` (2232 one-instruction blocks) | `HLE_RANGE`/`HLE_FUNC`, declined when a slice end fell inside | `HLE_F_ATOMIC`: an interrupt cannot fall inside one instruction, so no decline; callable by `hle_callc` (rte excepted: it lowers the mask) |
| boot and VDP-poll census blocks (auto2.c 303, boot.c 49) | `HLE_RANGE`, 92652 frame-end declines | `HLE_RANGE_YS` (YIELD + SHORT): a coroutine only when an event is inside the declared span; the "this block opened the VDP session" flag is `hle_cor_slot`, per coroutine |
| VBlankInt $000308, ExtInt $00039E | `HLE_RANGE` (1 frame-end decline) | `HLE_RANGE_YS`, effects before charges |

Not done (not in this task): VramClearDma $00800C and DmaFillAt $00807C (`#if 0` in vdpfill.c, the other agent's); PalFade,
CramUpload and a few helpers are still called as nested 68k by non-coroutine C (0.16% of busy). `T2_NATIVE=1` is unchanged
(misc_t3 runs 254809 C blocks, 0 instructions on a 68k core; gun_01 / pad_05 / 2p_gun_06 still stop at VramClearDma).

## Idle (not work)

| addr | name | % of all | note |
|---|---|---|---|
| $0078A6 | FramePaceWait loop | 58.3 | `cmp.w ; bgt.s` until vblank_count reaches frame_target; GameLoop pieces stop before it |
| $0086E2 | WaitVBlank | 18.2 | waits for vblank_count to change (menus, fades, loads) |
| $01DC4E | VDP wait | 0.7 | spins on `(1,a6)` bit 3 then `$C00008` HV counter == 0 |

## P5 update: coroutines instead of declines (GitLab github/t2-decomp#11)

The functions below no longer decline or bail when they are `HLE_F_YIELD` coroutines (docs/porting.md,
"Yielding"): VramWriteWord, DigitPrint, HudScores, HudLives, StringPrint, TextPrint, PrintText, ScrText,
ScrTextClear, VramFillWords (+ the 873A / 874E loops), CramUpload, the VDP list (VdpListRun and all
VdpCmd0..8 entries: the argument prologues and the VramFillWords / VramWriteWord calls are C now), ScrollRows*,
BuildSpriteTable*, MissilesRun / MissilesSlot, PlayersButtons, PlayerButtons, PlayerShoot, EnergyHud,
GunpowerHud, the PURE functions with an exact timeline (their interrupt-window declines), the AI / on-hit
ranges of ai_*.c and onhit_*.c, and the 68k helpers they called, ported as C (callees2.c, callees3.c,
queues2.c, pickup2.c, hudtext2.c, bcdhelp.c): DONE. VramClearDma $00800C, DmaFillAt $00807C, DmaToVram
$011FA2, DmaToVramSave $011F94 and VdpVScrollWrite $0081B6 are C coroutines too (vdpfill.c): DONE.
Declines, bails and nested 68k of these are gone as well (w4_2, above).
Merged with the P5/P5b native runner: wide 92/92, route gates
identical, `T2_NATIVE=1` 51 of 92 states fully native (the others stop at VramClearDma).

## Already in C but declined (10.3% of busy, 2.3% of all; was 9.7% / 2.1% at the previous measurement)

Not remaining port work: the original runs when a VBlank/HBlank would land inside the call, or the C charge is
booked as 68k (the figure includes the charge-only rows above, so it is an upper bound; it also grew because
the pieces that used to be 68k helpers are now C and are counted here when they decline). A function runs in C
up to the last bail point before the event and the 68k runs only the stretch around it; the loops take over again
at the next loop head (docs/porting.md "Early bail and resume"). Same 16 runs / 57000 frames
(`T2_HLE_DECLINES=1` with `T2_HLE_PROFILE=1`), declines per function (the `declines:` lines, summed; a bail is not a
decline, the `bails:` lines count them: PlayersButtons 24320, MissilesRun 6140, MissilesSlot 5573,
BuildSpriteTable 4498, EnergyHud 1769, GunpowerHud 1656, ScrollRows 1577, PlayerShoot 787, BgStreamLayers 300):

| function | declined | reason | note |
|---|---|---|---|
| VramWriteWord | 25786 (of 87278 calls) | other | nested guarded calls; 2.6 Mcyc real |
| VdpCmd1CopyWords (+ CopyLoop) | 10222 + 1024 vclr, 492 vint | VBLANK-clear/wrap | only the head runs as 68k, the copy loop is taken over at $0120B0 |
| HudScores, HudLives, DigitPrint, StringPrint, TextPrint | 7022, 5469, 5225, 2658, 1464 | other (guard) | the text printers decline when a line-start event can fall inside |
| BuildSpriteTablePiece, BuildSpriteTableSort | 5071, 4713 | interrupt window | bail/resume pieces; 7985 piece calls |
| VdpCmd2FillZero, VdpCmd0End, VdpCmd7HScroll, VdpCmd6TileRect, VdpCmd6Row, VdpCmd7HScrollSetup, VdpCmd6Words, VdpCmd8VScroll, VdpListRun | 4141, 3777, 3720, 3053, 2366, 2211, 1676, 1224, 4302 | VBLANK-clear/wrap | the list starts about 10 lines before the wrap |
| VramFillWords | 2838 of 4329 | other | 8.9 Mcyc real, VDP FIFO time |
| PrintText | 2742 | frame end | |
| PlayerDamageNearest, PlayerDamage, ScrollRowsRow, PlayerShoot | 1914, 1906, 1880, 1186 | interrupt window | one-off windows, each below 0.05% of busy |
| GunpowerHud, EnergyHud | 187, 210 of 44919 | interrupt window | the rest of their "real" time is the C charge (above) |

What is left is small and not worth more machinery: the VBLANK-clear/wrap declines (a short command that straddles the
event has nothing to resume, and the fills' time is VDP FIFO time, so resuming would need the VramFill loops split
like the copy loop, each ~0.7% of busy at most) and the one-off interrupt-window declines (10 to 24 thousand cycle
bounds for a path that is usually short, each below 0.05% of busy).

## Remaining 68k, by group

Mcyc = million 68k cycles over the 57000 frames; % is of busy. Difficulty: easy = work RAM / pure;
medium = VDP port I/O or nested calls (the vdplist.c pattern); hard = Z80 bus / blocking waits / large.

### Level loading and VDP uploads (1.2% of busy)

| addr | name | Mcyc | % | calls | what | difficulty |
|---|---|---|---|---|---|---|
| $00800C | VramClearDma | 12.3 | 0.95 | 20 | DMA fill of VRAM; polls DMA busy, ~660k cycles per call | left 68k: see below |
| $011FA2 | DmaToVram | 3.1 | 0.24 | 153 | 68k to VRAM DMA | left 68k: see below |

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

### Other (0.5% of busy)

The work-RAM leftovers and the former rows of this table (ObjPathStep, PlayerState, VdpQueueTileRect, PickupsRun,
the VdpList helpers, SpriteQueueReset, the event ring consumer, BgStreamL1..L5, the object / truck / HUD-bar helpers,
the rts stubs, the per-player state handler) are C now (game/misc2.c, vdphelp.c, ringevt.c, helpers.c, rtsstubs.c,
playerstate.c) and are dropped from this list.

| addr | name | Mcyc | % | calls | what | difficulty |
|---|---|---|---|---|---|---|
| $00B3AE | (object loop dispatch) | 4.4 | 0.34 | 725 | `jmp (a0)` into object handlers; structural | n/a (indirect jump; ObjRunAll owns it) |
| $009054 | (HUD helper) | 0.57 | 0.04 | 5909 | | easy: pieces between the jsr's |
| $012D00 | (pickups helper) | 0.48 | 0.04 | 766 | also called nested from C (0.9 Mcyc) | easy |
| $00C3C6 | (object helper) | 0.43 | 0.03 | 2508 | | easy |
| $011D40 | VdpListJump | 0.39 | 0.03 | 37827 | VdpList helper | easy: C caller could call a body directly |

## Suggested order

What is actually left, by % of busy (everything else in the lists above is C now or is declined C):

1. VramClearDma $00800C: 0.95% (DMA wait, five frames of polling; not lockstep-exact in C, see above).
2. The VBLANK-clear/wrap declines: VramFillWords 0.69% (8.9 Mcyc real), VdpListRun / VdpCmd* heads about 0.2% together,
   VramWriteWord 0.20%; would need the fill loops split like the copy loop.
3. $00B3AE object loop dispatch: 0.34% (indirect `jmp`, structural; ObjRunAll owns it).
4. DmaToVram $011FA2: 0.24% (DMA length halts the 68k per line; same reason as 1).
5. The small helpers $009054, $012D00, $00C3C6, VdpListJump $011D40: 0.04 + 0.04 + 0.03 + 0.03 = 0.14% together.
6. StoryIntro $0042CE: under 0.04% (blocking waits).
7. The C charge of GunpowerHud (1.86%), EnergyHud (1.38%), GameLoopBg6 (0.87%), GameLoopGunpower (0.33%) is booked as
   68k at the trap address; it is not port work (about 4.4% of busy is that bookkeeping, not 68k instructions).
