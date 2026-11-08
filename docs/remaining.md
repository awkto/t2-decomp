# What still runs as 68k with `--hle all`

Measured with `T2_HLE_PROFILE=1` (docs/porting.md): 16 runs, 57000 frames, `--hle all`, lockstep input
script. The runs are the six default states, pad_05, 2p_gun_04, gun_03, gun_08, hk_a, sky_b, l7_chase,
misc_t3 (3000 frames each), the title state left alone for 6000 frames (attract loop), and a cold boot
for 9000 frames. Merge with `port/tools/profmerge.py`. Cycles are 68k cycles; "busy" = everything but idle.
Lockstep `--frames 1000` (six defaults) is identical with the variable set.

## Totals

Measured twice on the same 16 runs: first after misc2.c, the VDP list bail, textprint.c, vdpfill.c and sound.c
(before the early bail), then after the early bail and resume (docs/porting.md "Early bail and resume").
The tables further down are the second measurement.

| | % of all frame time | % of busy | before the early bail: all / busy |
|---|---|---|---|
| idle (waiting) | 78.9 | - | 77.7 / - |
| C replacements (charged) | 18.0 | 85.0 | 15.6 / 69.9 |
| original 68k | 3.2 | 15.0 | 6.7 / 30.1 |

- The C share includes 68k that C calls itself (VramWriteWord and the text printers run nested): 1.4% of
  all, 6.4% of busy. Pure C is 16.6% of all, 78.6% of busy. So 21.4% of busy work is still 68k instructions
  (real 68k plus the nested part).
- Of the 15.0% real 68k, 9.7 points (2.1% of all) are routines that are already in C but were declined (an
  interrupt falls inside the call, docs/porting.md "Interrupts inside a replaced function") and ran as the
  original. The rest, 5.3 points (1.1% of all), is code with no replacement yet. Rows below are that part.
  The "real 68k" of an HLE function is an upper bound: a C call that charges more than one stall slice is burned
  at a trap address and the profiler books those cycles to the function (GunpowerHud and EnergyHud, 21.5 and
  16.0 Mcyc, are their ~450-cycle C charge, not 68k work: they now decline 185 and 197 times in 41763 calls).
- Per run, C is 66-92% of busy (boot 66%, attract 72%, title 80%, misc_t3 80%, gun_01 82%, gun_05 92%).
- The profile is mostly in-level frames: one-shot level loading and the menus barely register (the
  attract loop is 92% idle). Their cost is blocking structure, not CPU.

## Idle (not work)

| addr | name | % of all | note |
|---|---|---|---|
| $0078A6 | FramePaceWait loop | 51.9 | `cmp.w ; bgt.s` until vblank_count reaches frame_target; GameLoop pieces stop before it |
| $0086E2 | WaitVBlank | 23.0 | waits for vblank_count to change (menus, fades, loads) |
| $01DC4E | VDP wait | 1.0 | spins on `(1,a6)` bit 3 then `$C00008` HV counter == 0 |

## Already in C but declined (9.7% of busy, 2.1% of all; was 25.2% / 5.6%)

Not remaining port work: the original runs when a VBlank/HBlank would land inside the call. Most of the
declines are gone: a function now runs in C up to the last bail point before the event and the 68k runs only the
stretch around it; the loops take over again at the next loop head (docs/porting.md "Early bail and resume").
Same 16 runs / 57000 frames (`T2_HLE_DECLINES=1` with `T2_HLE_PROFILE=1`), declined / calls, before -> after
(a bail is not a decline; the `bails:` lines of `T2_HLE_DECLINES` count them):

| function | before | after | what |
|---|---|---|---|
| BuildSpriteTable | 4085/41763 | 80/38840 | bail at the sort / sprite / piece / tail heads, resume there (4031 bails); real 68k 79 Mcyc -> about 1 over all its entries |
| GunpowerHud, EnergyHud | 1603, 1689 /41763 | 185, 197 /41763 | bail before each `jsr`; the heavy part is the C StringPrint under its own guard |
| BgStreamLayers | 107103/121667 | 407 (266 bails) | light passes in C, a pass whose trigger fired bails when the nested VdpQueue call would not fit |
| PlayerShoot, PlayersButtons, PlayerButtons | 27974, 21407, 11661 | 1074, 243, 1050 | bail at the `jsr`/`bsr` sites; PlayersButtons mostly bails at its first call and the callees run as their own hooks |
| MissilesRun | 17982/43549 | 56 | bail at the slot head and before a live slot's state body, resume at the slot head |
| ScrollRows | 1413/40461 | 28 | bail at the group / row / list heads, resume there |
| VdpCmd1CopyWords | 8786/33018 | 8786 (head), resumed | head still declines near VBLANK-clear/wrap, but the 68k only does the head: the copy loop is taken over at $0120B0 (real 68k 81 -> 1.8 Mcyc) |
| VdpCmd7HScroll | 3372/3921 | 3372 (head), resumed | same, resume at $011E7E / $011E8A / $011EB4 (27 -> 0.5 Mcyc) |
| VdpCmd6TileRect | 4491/18491 | 2921 | now bails in its row / word loops and resumes at $0120DA / $0120F6 (real 68k 7.0 Mcyc -> below 0.4) |
| VdpListRun | 4089/47748 | 4049 | the first command's head did not fit before VBLANK-clear/the wrap; unchanged |

What is left is small and not worth more machinery, in order of the real 68k Mcyc in the table below:
- GunpowerHud / EnergyHud / sub_00920C: ~0.4% declined; the rest of their "real" time is the C charge (see above).
- VramFillWords 2827/4250 (9.0 Mcyc), VdpCmd2FillZero / VdpCmd3WriteWord / VdpCmd0End / VdpCmd4CramLine / VdpCmd8VScroll (1 to 4 thousand each) and the
  nested VramWriteWord / DigitPrint / StringPrint (25k, 5k, 2.5k): they sit at the VBLANK-clear/wrap boundary
  (the list starts about 10 lines before the wrap) or are guarded `hle_call68k` fills; a short command that
  straddles the event has nothing to resume, and the fills' time is VDP FIFO time, so resuming would need the
  VramFill loops split like the copy loop (each ~0.7% of busy at most).
- PlayerDamage, PlayerDamageNearest, ShotHitTest, TruckMain and the other one-off interrupt-window declines (a few
  hundred each): bounds of 10 to 24 thousand cycles for a path that is usually short; the same call-site bail would
  apply, but each is below 0.05% of busy.

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
| $013376 | SpriteQueueReset | 4.2 | 0.32 | 44061 | resets sprite queue | DONE in C (game/vdphelp.c) |
| $011D38 | VdpListNext | 4.2 | 0.32 | 127128 | VdpList helper, 3 instructions | DONE in C (game/vdphelp.c) |
| $013F7E | (event ring consumer) | 4.1 | 0.29 | 43549 | drains byte-pair ring at $FF80A0 (head $FF80B0, tail $FF80B2); Random, VdpQueueTileRect, QueueSprite | DONE in C (game/ringevt.c): pieces between the jsr's, with the producer $013E30 and the text rows $0141B8/$0141E6 |
| $011D12 | VdpListReset | 3.5 | 0.27 | 47757 | VdpList helper | DONE in C (game/vdphelp.c) |
| $011D28 | VdpListEnd | 2.4 | 0.18 | 47757 | VdpList helper | DONE in C (game/vdphelp.c) |
| $004916, $0048C4, $004820, $004872 | BgStreamL1, L4, L5, L2 | 2.7, 1.8, 1.3, 0.6 | 0.21 to 0.05 | 11659, 8773, 6336, 2943 | per-layer wrappers that `bsr` into BgStreamLayers (C) | easy; the GameLoopBg* pieces cover the calls from the loop |
| $00DB36 | (object helper) | 1.4 | 0.11 | 11003 | | easy |
| $013188 | (sprite queue helper) | 1.2 | 0.09 | 10052 | calls the object animation step $00C48E | DONE in C (game/pools.c MissileTail) |
| $00D238 | (truck helper) | 1.0 | 0.07 | 3714 | | easy |
| $00920C | (HUD bar helper) | 0.9 | 0.07 | 3198 | builds the bar string, then StringPrint (C) | easy |
| $00B3C6, $009356, $00C442 | (object loop, script tick, animation helpers) | 0.7, 0.7, 0.7 | 0.06 to 0.05 | 44059, 43562, 4347 | | easy |
| $008C8E, $008C52 | (per-player state handler) | 11.8, 3.4 | 0.82, 0.23 | 87127 | a4 = $FF8000/$FF8050 player struct, by state word +$22: timers, sounds, ReadInput, WaitFrames | DONE in C (game/playerstate.c): pieces between the jsr's (blocking waits, sounds, ReadInput stay the real 68k calls); the PlayerDied tail was already screens.c |

## Suggested order

1. (mostly done) The declines: bail at the event and resume at the next loop head (25.2% of busy declined before, 9.7% now,
   see "Already in C but declined").
2. (done) Text/HUD printing with VramWriteWord: game/textprint.c.
3. (done: ObjPathStep, PickupsRun, VdpQueueTileRect, PlayerState idle path in game/misc2.c)
4. (done) Sound engine tick: game/sound.c. The boot-time Z80 loader ($0FBB00 and callees) is the only sound code left.
5. (done) VDP upload helpers: game/vdpfill.c. VramClearDma and DmaToVram stay 68k (DMA timing).
