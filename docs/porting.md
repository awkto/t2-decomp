# Porting 68k functions to C (HLE) and difftest

`t2port` runs the original ROM on a statically linked Genesis Plus GX core. Functions can be
replaced with C one at a time ("HLE"); each replacement is proven equivalent to the original
68k code by `difftest` before it is trusted. The machinery is the one from the sister project
scooby-decomp.

```
port/
  hle/hle.h          THE API for decompiled code: HLE_FUNC, ctx, RAM/ROM/bus accessors, flags
  hle/hle.c          registry, CPU dispatch (via tools/patches/gpgx-hle.patch), nested 68k calls
  hle/gx.h, gx.c     the only bridge to GPGX internals (memories, 68k state, VDP/Z80)
  hle/sandbox.c/.h   run one function in isolation and snapshot/compare machine state
  src/capture.c/.h   entry-state capture (savestate when the PC reaches an address)
  src/emu.c          core wrapper; also the emu_add_hook fan-out over the core's cpu_hook
  game/<subsystem>.c decompiled game code (one file per subsystem); game/random.c is the first
  tools/difftest.c   differential tester
  tools/dt           one-command build + capture + difftest
  scripts/capture.txt capture route (level 5 state + mouse play)
```

At configure time CMake applies `tools/patches/gpgx-cpuhook.patch` (HOOK_CPU) and
`tools/patches/gpgx-hle.patch` (the `m68k_hle_*` override hook) to the exported GPGX copy in
`build/port/gpgx-src`; `ext/gpgx` is never modified. `HOOK_CPU`/`HOOK_HLE` are in
`port/cmake/gpgx.cmake`.

## Running with C replacements

Every registered replacement starts disabled, so plain `t2port` (and `--hle none`) runs the
original 68k code. `--hle` selects what runs as C:

    build/port/t2port                                   # pure emulation (default: nothing enabled)
    build/port/t2port --hle none                        # same, explicit
    build/port/t2port --hle all                         # every registered replacement
    build/port/t2port --hle Random,MenacerReadXY        # just these two
    build/port/t2port --hle all,-Random                 # everything except one function
    build/port/t2port --options hle=all                 # same, through the ini/option system
    build/port/t2port --options hle=Random+MenacerReadXY  # a list there: '+' separates names
    build/port/t2port --hle-list                        # print the active set and exit

Names may be separated by `,` or `+`; a leading `-` turns a single one off. An unknown name is
an error (exit status 2). `--options` splits its value on commas, so a list given through it
must use `+` (as above); the ini form is the same (`hle = all` or `hle = Random+MenacerReadXY`).
`hle` is a normal option (default `none`), so it can also go in
`~/.config/t2-decomp/t2.ini`. At startup t2port prints the active set on stderr, e.g.
`hle: 2 active: MenacerReadXY ($01E442), Random ($00881C)` (or `hle: 0 active: (none)`).

The registry holds `Random` ($00881C), `MenacerReadXY` ($01E442), `ReadPad1` ($0088A0),
`ReadPad2` ($008908), `ReadInput` ($0003DE), `ReadInputAim` ($0004B6), `PadMoveCrosshair`
($0005FC), `QueueCrosshair` ($00071C),
`AddScoreBcd` ($007376), the `AddScore250..10000` entries
($0072F8..$007368), `BcdAdd` ($007A92), and the palette path `PalFadeStep` ($008970),
`Copy32` ($008A7C), `PalCycle` ($00E9FA), `PalFade` ($008A5E, the loop body after the
two WaitVBlank calls; see "The palette path" below),
and the sprite queue `QueueSprite` ($013240), `ObjQueueSprite` ($013228) and `BuildSpriteTable` ($013272),
and the hit-test path `ShotHitTest` ($005892), `HitBox` ($005940) and `PickupHitBox` ($007086).


`MenacerReadXY` also serves `aim=fast`: when `hle_aim_fast()` is set (by `emu_set_aim_fast`,
before the CPU is hooked, so it agrees with the in-memory ROM patch) it runs the $01E460 patch
behaviour (newest latch only) and charges the patch's ~250 cycles; otherwise it runs the original
8/16-latch average and charges 1588. `difftest --aim game` (default) compares against the
unpatched 68k, `--aim fast` against the patched 68k.

## The C API (`port/hle/hle.h`)

```c
#include "hle.h"

/* $00881C: LFSR-ish RNG step, returns the low word of the new state in d0.lo */
HLE_FUNC(0x00881C, Random, 192)
{
    uint32_t rng = RAM32(0xFFA0F0);
    ...
    set_w(&ctx->d[0], nd);
    cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C, flags);
}
```

- `HLE_FUNC(addr, name, cycles)` defines `void name(hle_ctx *ctx)` and registers it with a
  constructor. A new `.c` file under `port/game/` is picked up automatically (re-run cmake so
  the glob sees it). `cycles` is the original's average 68k cost (difftest prints it).
- The CPU calls the function when it reaches `addr`. Afterwards the layer performs the `rts`
  (pops the return address) unless the function calls `hle_jump(ctx, addr)`.
- `ctx->d[8]`, `ctx->a[8]` (a[7] = SP), `ctx->sr`. Partial writes: `set_b`/`set_w`.
- Flags: `cc_set(ctx, mask, bits)`, `cc_logic8/16/32`, `lsr16`, `lsl8`. Add helpers as needed.
- Memory: `RAM8/16/32`, `RAM32_W`, `ROM8/16/32`, `bus_r*/bus_w*` (any address, with the
  hardware side effects), `vdp_ctrl_w`/`vdp_data_w`/... All port I/O must go through these so
  difftest sees it.
- Callees not ported yet: `hle_call68k(ctx, addr)` runs the 68k code as a `jsr`.
- Mirror every register and flag the original leaves behind, even ones that look unused.

## Replacing one function, end to end

1. **Pick and read it.** Find the label in `src/t2.s` (and `data/symbols.txt`). Note the
   callers, the exits (all `rts`, any tail `jmp`/`bra`), the registers it saves/clobbers, and
   the final flags (the last flag-setting instruction before `rts`).
2. **Write it** in `port/game/<subsystem>.c` with `HLE_FUNC(addr, name, cycles)`.
3. **Difftest it:** `port/tools/dt <name>` builds `difftest`, captures >=200 entry states into
   `runs/states/<ADDR>/` if missing (playing `port/scripts/capture.txt`), then runs every state
   twice - the original 68k code until it returns, and the C function - and compares D0-D7/A0-A7,
   SR (X N Z V C), the inactive SP and return PC, work RAM (minus scratch stack), VRAM/CRAM/VSRAM,
   VDP registers + port latches, Z80 RAM/bus, I/O registers, and the ordered list of writes to
   the VDP/Z80/I-O areas. Iterate until `name $ADDR: 200/200 states equal`.
4. **Play-test it:** run a route with `--hle all` and with `--hle none` and check the game still
   behaves (e.g. `port/tools/mousetest.sh build/port/t2port flow`, and the same flow with
   `--hle all`).

`dt` / `difftest` options: `--states DIR`, `--max N`, `--n N` (states to capture),
`--script FILE` (capture route), `--every K`/`--skip K`, `--ignore-regs d1,a0` and
`--ignore-flags` (only with a proof in the C source that no caller reads them), `--strict-stack`,
`--with-hle`, `--passthrough` (C side calls the original: self-test), `--poke "W:ADDR=VAL,D0=7"`
(run each state once per variant), `--aim game|fast` (default game: the reference is the unpatched
68k; fast patches MenacerReadXY's body in memory as t2port does), `-v`, `--keep-going`. `difftest
list` prints the registry, `difftest check all` tests everything.

## The capture route

`port/scripts/capture.txt` loads a harness savestate (`states/gun_05.state`, not committed) and
plays ~6800 frames with the mouse, which calls `Random` ~210 times. `dt` first counts the calls
on the route and keeps every k-th so the captured states are spread out, then captures 200.
The capture loop feeds the scripted mouse through `mouse_frame()` as the real frame loop does, so
the Menacer actually latches and `MenacerReadXY` sees gun-on states (both the 8-sample moving and
16-sample still branches: 13/187 of the 200). `port/scripts/capture_menacer_off.txt` alternates
aiming and `mouse leave` to also cover the gun-off early exit (`gun_latch_count == 0`):

    build/port/difftest capture 01E442 200 --script port/scripts/capture_menacer_off.txt --out runs/states/01E442_off
    build/port/difftest run MenacerReadXY --states runs/states/01E442_off      # and --aim fast

## Result so far

| Address | Name | File | difftest |
| --- | --- | --- | --- |
| $00881C | Random | game/random.c | 200/200 states equal (a deliberate off-by-one is caught) |
| $01E442 | MenacerReadXY | game/menacer.c | 200/200 states equal, `--aim game` and `--aim fast` (a deliberate wrong divisor is caught) |
| $0088A0 | ReadPad1 | game/pads.c | 200/200 states equal (a deliberate missing `not.b` is caught) |
| $008908 | ReadPad2 | game/pads.c | 200/200 states equal (a deliberate missing `not.b` is caught) |
| $0003DE | ReadInput | game/input.c | 200/200 states equal (mode 2 also 200/200 with `--poke B:FF9DF5=2`) |
| $0004B6 | ReadInputAim | game/input.c | 200/200 states equal (a deliberate wrong button nibble `$f0`->`$e0` is caught) |
| $0005FC | PadMoveCrosshair | game/input.c | 200/200 states equal |
| $00071C | QueueCrosshair | game/input.c | 200/200 states equal |
| $007376 | AddScoreBcd | game/score.c | 270/270 states equal (P1=200, P2=70; 63 with a BCD carry; the skip path 270/270 with `--poke W:FF8024=0xFFFF,W:FF8074=0xFFFF`) |
| $0072F8..$007368 | AddScore250..10000 | game/score.c | 200/200 for 250/500/750; 78/78, 79/79, 12/12, 3/3, 2/2, 1/1 for 1000/5000/4000/2000/1250/10000 (their real call counts; the same 14-byte stub, proven by AddScoreBcd) |
| $007A92 | BcdAdd | game/score.c | 200/200 states equal (d3 = 1,2,6,7; a deliberate wrong carry fixup is caught) |
| $008970 | PalFadeStep | game/palette.c | 200/200 states equal (175 moved / 25 nothing-changed, changed entries 13..58; the `pal_fading = 0` early exit 200/200 with `--poke B:FFA246=0`; a deliberate step `2`->`4` is caught) |
| $008A7C | Copy32 | game/palette.c | 88/88 states equal (a deliberate 7 instead of 8 moves is caught) |
| $00E9FA | PalCycle | game/palette.c | 200/200 states equal (a deliberate wrong `vblank_count & $18` mask is caught) |
| $008A5E | PalFade | game/palette.c | 200/200 states equal (loop-back $008A4C 175 / rts $008A7A 25; a deliberate inverted exit is caught) |
| $013240 | QueueSprite | game/sprite.c | 600/600 states equal (180 cycles, constant) |
| $013228 | ObjQueueSprite | game/sprite.c | 600/600 states equal (278 cycles; calls the C `QueueSprite`) |
| $013272 | BuildSpriteTable | game/sprite.c | 1300/1300 states equal (a deliberate descending sort is caught; exact per-path cycle charge) |
| $005940 | HitBox | game/hittest.c | 600/600 states equal (21 hit / 579 miss; a deliberate box edge `<`->`<=` is caught at state 67) |
| $007086 | PickupHitBox | game/hittest.c | 687/687 states equal (2 hit / 685 miss; a deliberate box edge `<`->`<=` is caught at state 407) |
| $005892 | ShotHitTest | game/hittest.c | 200/200 states equal (no-hit 69 / hit 131, multi-hit sort 27, kill 55, front-not-shootable 7, human objects 4; a deliberate descending depth sort is caught at state 15) |

`QueueSprite`/`ObjQueueSprite`/`BuildSpriteTable` are in `game/sprite.c` (docs/subsystems/sprite.md).
`QueueSprite` (`$013240`) appends one entry to `spr_queue_args` `$FFA318` and an (index, priority)
pair to `spr_queue` `$FFA250`; `ObjQueueSprite` (`$013228`) is the object form (X/Y from `(a6)`,
priority = depth `(a6+7)`) and, since the C `QueueSprite` is proven equivalent, calls it directly
(the nested call's registers/flags/RAM are exactly the `jsr`'s). `BuildSpriteTable` (`$013272`)
reproduces the stable bubble sort of the pairs by the priority byte (equal priorities are NOT
swapped), the `SpriteMappings` `$0152A8` expansion, the `bset.b #7`/`bclr.b #7` priority-bit rules
(`sprite_prio_depth` `$FF9EB8`, Y >= `$141`), the 79-piece cap and the list command 1
(count `d6*4` words, `spr_table` -> VRAM `$F400`) queued at `vdp_list_ptr` `$FF80BC`; an empty queue
writes one dummy entry (Y and link only). It mirrors every clobbered register (a0-a5, the high words
of d2/d3/d5, GPGX's `movem.w` sign-extension into the whole register) and the final flags
(`move.l a5,(vdp_list_ptr).l`: N/Z from a5, V=C=0; X from the `asl.w #2,d6`).

Cycles: `QueueSprite`/`ObjQueueSprite` are straight-line (180/278). `BuildSpriteTable`'s cost varies
by path (258 empty .. 58618 on the captures), so its charge is summed per instruction as it executes
(GPGX 68000 table + movem/shift/bcc/dbcc extras; `T2_SPRITE_STATS=1` prints the split). It matches the
sandbox-measured original exactly on 1298/1300 states and within -112/-18 cycles on the other two
(GPGX's memory-refresh `+2` every 128 cycles, which depends on the absolute cycle phase). Captures:
the default `capture.txt`, `capture_score.txt`/`capture_score2p.txt` (the busy pad-bot runs; 2P hits
the 79-piece cap) and `capture_bcdadd.txt` (the level-skip run reaches the empty-queue path), merged
into `runs/states/013272`; `port/scripts/capture_sprite_prio.txt` forces `sprite_prio_depth = 1` so a
crosshair piece (priority 1) sits exactly on the `<= depth` boundary, which normal play (depth 14..16)
never reaches. A deliberate `<=` -> `<` bug there is caught only with that capture; a descending-sort
bug is caught by 902/1200 of the default states. `hle.c`'s `STALL_SLICE` is 256 (below the ~488-cycle
scanline): a 1000-cycle slice overshoots the line boundary, so the next `m68k_run()` returns early
(`cycles >= cycle_end`) and skips its interrupt check - with `BuildSpriteTable`'s large charge that
dropped the Menacer TH latch on the gun's line and broke `mousetest grid`'s aim (the same slicing
problem `game/menacer.c` works around for `MenacerReadXY`). difftest is unaffected (the sandbox never
slices).

`AddScoreBcd`/`AddScore*`/`BcdAdd` are in `game/score.c`. `AddScoreBcd` (`$007376`) adds a 4-byte
BCD value to `p1_score`/`p2_score` (`$FF9EA8`/`$FF9EB0`); the destination is chosen by the player
index at `(a4+$24)` (negative = skip, zero = P1, else P2). The four `abcd -(a1),-(a0)` bytes
reproduce GPGX's `m68k_op_abcd_8_mm` exactly: X=C=decimal carry, Z only ever cleared, and the core's
"undefined" N/V formulas (`V = (~r2) & r3`, N from bit 7 of the result). Note the frame: the 14-byte
entry stubs (`$0072F8`..`$007368`: `movem.l d0/a0-a1,-(sp)` + `move.l #VALUE,d0` + `bra.w
AddScoreBcd`) push d0/a0/a1 and `AddScoreBcd` pops them, so a state captured at `$007376` has the
stubs' frame at SP and the routine leaves with SP = entry+16. difftest's plain-function model (return
at `[SP]`, exit SP = entry+4) does not fit, so `AddScoreBcd` is an `HLE_RANGE` that stops at `$00739A`
(just before the epilogue `movem/rts`, which then runs as 68k); the stubs call the shared C body
directly. `$007306` (AddScore500) is referenced nowhere in the ROM (data/functions.txt, xref): it is
dead code, so no state can be captured at its address. Its `runs/states/007306` dir holds the
`$0072F8` captures with the saved entry PC redirected to `$007306` (the GPGX m68k PC is at byte
offset 144468 of the state blob, little-endian; the same redirection `--entry` performs), so a plain
`dt AddScore500` works; the calling context is identical and only the immediate differs.

`BcdAdd` (`$007A92`) is not BCD addition: it converts d4 into `d3+1` ASCII decimal digits at
`$FFA0BA` (most significant first, NUL-terminated), calling the helper `sub_007AD4` (`d0 *= 10`)
`d3` times; that helper is inlined in the C version. Only `$FFA0BA..` and the flags are observable
(d0-d7/a1 are saved/restored), and the last flag-setting instruction is `clr.b` (N=0,Z=1,V=C=0).

Captures. `port/scripts/capture_score.txt` (level 5, 1P joypad `pad_05`) and
`port/scripts/capture_score2p.txt` (2P mode 0, `2p_gun_05`, P1 mouse off + the P2 pad replayed) are
the recorded input of the headless harness' pad-bot (`botcfg p1 pad1` / `p2 pad1` with `GUN=1` on
port 2), replayed through the port's `press` commands; both runs score many kills. The two pad-bot
routes give AddScoreBcd 312 (1P) and 70 (2P, all the a4=P2 branch) calls. `port/scripts/capture_bcdadd.txt`
is a level-skip run through levels 1..8 whose end-of-level tally screens call BcdAdd 208 times
(d3 = 1,2,6,7). The `T2_SCORE_STATS=1` env prints the P1/P2/skip/carry and d3 counts of a run.


`ReadPad1`/`ReadPad2` share `read_pad()` in `game/pads.c`: the Z80 bus request/BUSACK spin, the
TH=0 and TH=1 reads of the port data register, the active-low invert, the button word/previous
word writes and the final `SR = $2000`. The capture routes are `port/scripts/capture_pad.txt`
(1P joypad, level 5, scripted pad-1 holds) and `port/scripts/capture_pad2.txt` (a 2 JOYPAD state;
the emulator always puts a Menacer on port 2, which answers the same read with changing data).

## The input path (`game/input.c`)

`ReadInput` ($0003DE) and `ReadInputAim` ($0004B6) dispatch on `control_mode $FF9DF5`; the
per-frame path is `ReadInputAim` -> `PadMoveCrosshair` ($0005FC, pad acceleration table
`$000738`, X 8..312, Y 24..200) or the Menacer X/Y, then `QueueCrosshair` ($00071C) ->
`QueueSprite` ($013240). The C versions call the ported `ReadPad1`/`ReadPad2`/`MenacerReadXY`
directly (proven equivalent, so the nested call's registers, flags, RAM and I/O writes are
identical to the `jsr`); `QueueSprite` is not ported yet, so it is reached through
`hle_call68k`, which runs the 68k leaf exactly as the original `jsr` would. `PadMoveCrosshair`
falls through into `QueueCrosshair` in the original and ends by calling the C `QueueCrosshair`.

Capture routes (`port/scripts/`): `capture_input_gun.txt` (mode 4, 1P Menacer + scripted pad 1),
`capture_input_pad.txt` (mode 5, 1P pad), `capture_input_2p.txt` (mode 0, Menacer + pad 1),
`capture_input_2ppad.txt` (mode 1, two pads), `capture_input_dirs.txt` (long single-direction
holds so the acceleration index walks 0..18 and the clamps are reached), `capture_input_mode2.txt`
(mode 2, reached by poking `$FF9DF5`) and `capture_input_buttons.txt` (Menacer buttons). The
states are at the level intro; each route runs ~900 frames first so the player is in (index >= 0).
`ReadInput` is only called from the pause loop and the continue countdown, so
`capture_input_pause.txt` pauses every mode with Start (the gun's middle mouse button or pad 1)
and `capture_input_continue.txt` (played after `menuflow.txt`) dies into the continue screen.
On the default `dt` states the paths split: ReadInputAim mode2/twopad/mode04 = 5/99/96; ReadInput
twopad/mode04 = 91/109; PadMoveCrosshair directions up/down/left/right/none = 108/19/100/16/40;
QueueCrosshair queued/skipped = 98/102. `T2_INPUT_STATS=1` prints these counts at exit.

Reproduce the states (each `capture` writes `runs/states/<ADDR>/`):

    build/port/difftest capture 0004B6 200 --script port/scripts/capture_input_gun.txt \
        --script port/scripts/capture_input_pad.txt --script port/scripts/capture_input_2p.txt \
        --script port/scripts/capture_input_2ppad.txt --script port/scripts/capture_input_dirs.txt \
        --script port/scripts/capture_input_mode2.txt --script port/scripts/capture_input_buttons.txt
    build/port/difftest capture 0005FC 200 --script port/scripts/capture_input_dirs.txt
    build/port/difftest capture 00071C 200 --script port/scripts/capture_input_dirs.txt \
        --script port/scripts/capture_input_gun.txt --script port/scripts/capture_input_pad.txt \
        --script port/scripts/capture_input_2p.txt --script port/scripts/capture_input_2ppad.txt \
        --script port/scripts/capture_input_mode2.txt --script port/scripts/capture_input_buttons.txt
    build/port/difftest capture 0003DE 200 --script port/scripts/capture_input_pause.txt
    # the continue countdown, an extra ReadInput route (needs menuflow first for `l1`):
    build/port/difftest capture 0003DE 200 --script port/scripts/menuflow.txt \
        --script port/scripts/capture_input_continue.txt --out runs/cont_states
    port/tools/dt ReadInput ReadInputAim PadMoveCrosshair QueueCrosshair
    build/port/difftest run ReadInput --poke B:FF9DF5=2       # mode 2

## The palette path (`game/palette.c`)

`PalFadeStep` ($008970) walks the 64 colours of `pal_target`/`pal_cur`
(`$FFA134`/`$FFA1B4`), moves each 3-bit channel (B `$E`, G `$E0`, R `$E00`) one CRAM level
(2) towards the target, flags the changed entries in `pal_changed` (`$FFA0F4`) and clears
`pal_fading` (`$FFA246`) when nothing changed. It is a plain leaf; the C version mirrors
every register (d2-d5/d7 written with `move.w`, so their high words survive; d6 from
`moveq`/`move.w`, high word 0; d1 from `moveq #0`; d0 = `$FFFF` after the dbra) and the
final CCR (`tst.w d1`, or the closing `sf.b`). Its cost varies 4x with the number of changed
colours (original min/avg/max 3454/10626/15466 over the 200 states), so the charge is per
path: `3454 + 172*d1 + 12*ch` (changed entries `d1`, changed channels `ch`), which tracks
the original to ~25 cycles on every state (charged avg 10570 vs 10626); the early exit
charges 62.

`Copy32` ($008A7C) is eight `move.l (a0)+,(a1)+`; the C version reproduces the 8th move's
N/Z (`move.l` sets N/Z, V=C=0) and charges the constant 176.

`PalCycle` ($00E9FA) copies 8 bytes from the 4-step ROM table `$0084FA + (vblank_count &
$18)` into colours 12..15 of palette line 0 and calls `CramUpload`. It is registered as an
`HLE_RANGE` that stops right before the `jsr CramUpload` ($00EA1E), so the call runs as real
68k at the correct raster time: `CramUpload` polls the VDP status and returns its last read
in d1, and from C the nested call would start ~126 cycles early (the prelude is instant),
flipping the HBlank status bit in d1. With the split, d1 and the range are exact (200/200);
the `rts` stays 68k.

`PalFade` ($008A4C) is a blocking loop: `WaitVBlank`, `PalFadeStep`, `WaitVBlank`, a ~1430
cycle delay, `CramUpload`, until `pal_fading` clears (at most 7 steps). A C function cannot
wait for an interrupt (scooby-decomp's "Waiting for VBlank"), so the two WaitVBlank calls
and the `PalFadeStep` call stay 68k; the non-blocking body after the second wait
(`$008A5E`..`$008A7A`: delay, `CramUpload`, `tst.b pal_fading`, branch) is an `HLE_RANGE`
with exits `$008A4C` (loop back to the first wait) and `$008A7A` (the `rts`). `CramUpload`
is run through `hle_call68k`; the range carries only the ~1452 non-nested cycles (the layer
adds the nested call's ~3443, matching the original's constant 4895).

Captures (`port/scripts/capture_palette.txt`): each `load` of a level-intro state replays
the level load and its blocking fade-in (`mainloop.md`, NextLevel step 5), so 18 states
(gun_01..08, pad_01..04, 2p_gun_01..03, 2p_pad_01..02, ending_a) give 268 PalFadeStep /
PalFade calls. `capture_palcycle.txt` forces `pal_cycle_on` on (`poke FFA0A4 1 FF`) on three
level states; no shipped level turns it on by itself on the harness route. Path counts from
`T2_PALETTE_STATS=1`: PalFadeStep moved 175 / nothing-changed 25; PalFade loop-back 175 /
rts 25; the `pal_fading = 0` early exit is 0 on the route, so it is covered with
`--poke B:FFA246=0`. Reproduce:

    build/port/difftest capture 008970 200 --script port/scripts/capture_palette.txt
    build/port/difftest capture 008A5E 200 --script port/scripts/capture_palette.txt
    build/port/difftest capture 008A7C 200 --script port/scripts/capture.txt \
        --script port/scripts/capture_palette.txt
    build/port/difftest capture 00E9FA 200 --script port/scripts/capture_palcycle.txt
    port/tools/dt PalFadeStep Copy32 PalCycle PalFade
    build/port/difftest run PalFadeStep --poke B:FFA246=0    # the early exit

## The hit-test path (`game/hittest.c`)

`HitBox` ($005940), `PickupHitBox` ($007086) and `ShotHitTest` ($005892) are in `game/hittest.c`
(docs/subsystems/objects.md, "Hit testing, damage, kills"). `HitBox` tests `shot_xy` `$FF9E94`
against the box at a3 (x-off, y-off, w, h, relative to the object at a6+2/+4); on a hit it sets
d6 = -1 and appends (a6, a2) to `hit_list` `$FFA958` via a5 (a5 ends +8). `PickupHitBox` is the same
box test for the pickup pool (a6 = the pickup); on a hit it calls `sub_012AF4` (the pickup reaction)
and `sub_013E30` (the hit sound, d0 = the pickup type d6) and returns d6 = -1. `ShotHitTest` walks
`obj_table` `$FF87B2` (`obj_count` `$FF80DA` live slots, stride $2C, free slots skipped), calls every
live slot's +$1C hit test, bubble-sorts `hit_list` by the depth byte (+7) smallest first (stable:
equal depths are not swapped) and damages only the front-most entry (b15 set): `jsr onhit`,
`hp (+8) -= shot_damage` `$FFA0CA`, and if hp < 0 `jsr (a0)` again (the kill routine the on-hit
routine left in a0). Returns d5 = -1 if anything was hit, else 0.

Calls: nothing here calls a ported function directly. The per-object +$1C routines, the on-hit and
kill routines, and PickupHitBox's `sub_012AF4`/`sub_013E30` all go through `hle_call68k` (68k); when
`HitBox` is enabled the HLE dispatch inside the nested call runs the C version, exactly as the
original `jsr` would run the 68k one. Registers: every instruction is mirrored, including GPGX's
`movem.w` sign-extension into the whole register (HitBox/PickupHitBox d0-d3), the surviving high
words of `move.w` writes (ShotHitTest d5/d7, HitBox d2/d3), a3/a5 advancing and the final CCR (the
box tests' last flag setter is a `move.l`/`moveq`; X survives from the last `sub.w`/`add.w`, and
ShotHitTest's from its `sub.w`/nested call).

Cycles: charged per path, summed instruction by instruction with the GPGX 68000 table (plus the
movem/shift/dbcc extras). The nested calls' time is added by the layer, so ctx->cycles is only the
non-nested part. HitBox/PickupHitBox match the original exactly (HitBox min/avg/max 94/115/192,
charged 114 on 600 states; PickupHitBox 94/115/650, charged 114 on 687). ShotHitTest's total is
dominated by its nested calls (original 406/7678/12424); its non-nested share is charged 2675 avg.

Captures: `port/scripts/capture_hittest.txt` concatenates three recorded routes: level 1 (gun_01)
mouse play long enough for the type $13 humans and crates, the level-5 pad-bot kill run
(capture_score) and the 2P run (capture_score2p). Reproduce:

    build/port/difftest capture 005892 200 --script port/scripts/capture_hittest.txt
    build/port/difftest capture 005940 600 --script port/scripts/capture_hittest.txt
    build/port/difftest capture 007086 687 --every 1 --script port/scripts/capture_score.txt
    port/tools/dt ShotHitTest HitBox PickupHitBox

`T2_HITTEST_STATS=1` prints the path counts (object types seen, hit/miss/kill/sort). The
PickupHitBox hit path is rare (2 of 687 calls), so its captures are taken with `--every 1` over the
score route to include both hits; a deliberate box-edge bug is caught at state 407. A deliberate
descending depth sort in ShotHitTest is caught at state 15, and a HitBox box-edge bug at state 67.
Play-test: `port/tools/mousetest.sh runs/t2port_hle_all.sh` (a wrapper that runs `t2port --hle all`)
passes all six tests (flow, twop, continue, missile, grid, lag). A busy level-5 scene (a mid-level
savestate, after the intro fade) is byte-identical between `--hle all` and `--hle none`; the
level-intro frame differs only because of the pre-existing `PalFade` range, and the three hit-test
functions alone are identical at that frame.
