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
and the VDP command list `VdpListRun` ($011D32) with its handlers `VdpCmd0End` ($011D64),
`VdpCmd1CopyWords` ($012068), `VdpCmd2FillZero` ($011D70), `VdpCmd3WriteWord` ($011D90),
`VdpCmd4CramLine` ($011D9E), `VdpCmd5FillValue` ($011D80), `VdpCmd6TileRect` ($0120BA),
`VdpCmd7HScroll` ($011E48), `VdpCmd8VScroll` ($011F6E),
and the hit-test path `ShotHitTest` ($005892), `HitBox` ($005940) and `PickupHitBox` ($007086),
and the background scroll path `ScrollRows` ($004752) and `BgStreamLayers` ($00498A).


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
  the glob sees it). `cycles` is the original's average 68k cost (difftest prints it); for a
  function with an instruction timeline it is instead the longest time the call can take (see
  "The instruction timeline" and "Interrupts inside a replaced function").
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
5. **Lockstep gate (required).** difftest only proves one call's *state* equal; it does not check
   the cycle charge, which is what makes a long run drift. Run the gate on the function alone
   (below) and require "3000 frames identical" from the default states before trusting it.

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

## The lockstep gate (`port/tools/lockstep.sh`)

difftest compares the *state* after one call and is blind to the cycle charge; a charge that is
a few cycles off still shifts when the game finishes its frame relative to VBlank, and after
~20-80 frames `--hle all` no longer matches `--hle none` (the game's `vblank_count`, and then
its whole frame, is at a different point). The gate closes that gap:

    port/tools/lockstep.sh                          # --hle all, 3000 frames, six states
    port/tools/lockstep.sh --hle PalFade            # one function
    port/tools/lockstep.sh --hle Random --frames 600 --addr

It loads each state under `states/`, plays the same deterministic input (mouse aim/clicks + pad
buttons) for N frames with `--hle none` and with `--hle SPEC`, and compares, every frame, an
FNV-1a hash of **live work RAM** (everything below `$FFFFFC00`, i.e. excluding the system stack)
plus VRAM, CRAM, VSRAM and the VDP registers. `t2port --framehash FILE` writes the per-frame
hashes (`emu_frame_hash`, port/src/emu.c); two runs are deterministic, so the first line whose
combined hash differs is the first frame the builds diverge. `--addr` then re-runs both up to
exactly that frame with `--memdump FILE` and lists the differing bytes by region and address.

The stack is excluded because the HLE layer itself writes below SP (`hle_call68k`'s sentinel
return address, the `STALL_PC` continuation push): those dead-stack bytes differ between the two
runs even when the game state is identical, and without the exclusion every function "diverges"
at frame 1. The six default states are `gun_01 gun_02 gun_04 gun_05 gun_07 title`; the whole gate
(6 states × 3000 frames × 2 runs) takes ~30 s.

**Cycle charges and the bus refresh.** The charge is the sum of the original instructions' GPGX
68000 costs. Two systematic errors were found and fixed with the gate:

- **Wrong per-instruction costs.** `game/vdplist.c` had several: `move.w (An)+,(An)` is 12 (not
  8), `bset/bclr.b #n,(abs).l` is 24 (not 20), `move.w #imm,(d16,An)` is 16 (not 12), `asl.w #2,Dn`
  is 10 and `jmp (d8,pc,Xn)` is 14 (not 8/12); the jump-table `bra.w` and each handler's closing
  `bra.w VdpListNext` were not charged; and hscroll's `dbra` loop charged one iteration too many.
- **The 68k bus refresh.** The original adds 2 cycles every 128 cycles while executing
  instructions (`refresh_cycles`, m68kcpu.c); a lump C charge did not, so the clock ended short
  and the frame phase drifted. `run_def` now adds the refresh the C part would have accrued
  (period 882 mcycles after the first trigger) and advances `refresh_cycles`; `sess_end` rewinds
  the phase with the clock and returns the nested *base* cycles so refresh is counted once. The
  model is fine-grained (it assumes the instruction costs are added in arbitrarily small steps),
  so it can be off by the overshoot of an instruction at a refresh boundary. Functions that need
  to be exact keep an instruction timeline instead (next sections); the lump model remains for the
  rest.

**Result (non-VDP replacements).** Every non-VDP replacement is now lockstep-identical:

| spec | gun_01 | gun_02 | gun_04 | gun_05 | gun_07 | title |
| --- | --- | --- | --- | --- | --- | --- |
| each of `Random MenacerReadXY ReadPad1 ReadPad2 ReadInput ReadInputAim PadMoveCrosshair QueueCrosshair AddScoreBcd AddScore250..10000 BcdAdd PalFadeStep Copy32 PalCycle PalFade QueueSprite ObjQueueSprite BuildSpriteTable HitBox PickupHitBox ShotHitTest` alone, 3000 frames | identical | identical | identical | identical | identical | identical |
| `--hle all,-VdpListRun,-VdpCmd0End,...,-VdpCmd8VScroll` (all non-VDP together), 3000 frames | identical | identical | identical | identical | identical | identical |
| `--hle all` (VDP list included), 3000 frames | identical | identical | identical | identical | identical | identical |

Before (1000 frames): `ReadInputAim`/`PadMoveCrosshair`/`QueueCrosshair`/`MenacerReadXY`/`ReadPad1`/`HitBox`
diverged on `gun_05` at 845-846 (`ReadPad1` also on `title` at 733); `QueueSprite`/`ObjQueueSprite`
on `gun_05` at 56 and `BuildSpriteTable` at 963; `PalFade` on `gun_05` at 16 and `title` at 863,
`PalFadeStep` on `title` at 863, `Copy32` on `title` at 81. `T2_HLE_ORIG=name` runs a replacement
as the original 68k inside the full frame loop and logs its real cost next to the C charge
(`T2_HLE_CHARGE=1` logs the charge); it is a distorted environment (nested session), so compare
timelines with `port/tools/seqcmp.py` (below) instead of its per-call deltas.

## The instruction timeline (exact cycle charges)

A lump charge (`ctx->cycles = N`) is only as good as N: it puts the CPU clock within a few cycles
of the original, but the clock is not all that has to match. Four things inside a function depend
on *when* each original instruction would have run, and each broke the gate:

1. **The bus refresh phase.** The core takes the 2-cycle refresh at the start of the first
   instruction at or after `refresh_cycles` and sets the next one 896 mclk after *that* clock, so
   where the instruction boundaries fall decides every later refresh. A lump, even with the exact
   total, assumes the refresh fires exactly at the threshold: the phase drifts by the overshoot (up
   to the length of an instruction) per refresh, and a few frames later an HV counter read or the
   VBlank is a few cycles off. `movem.l` (and shifts/rotates by a register count, mul/div) also
   re-check the threshold *inside* the instruction after their per-register part
   (`SKIP_BUS_REFRESH`, m68kops.h) and skip one refresh without adding 2 cycles; `movem.w` does not.
2. **Timing-sensitive accesses.** `ReadPad1/2` request the Z80 bus; GPGX then runs the Z80 up to
   the 68k clock (`gen_zbusreq_w`), and a Z80 read of 68k ROM stalls the 68k by ~10 cycles
   (`memz80.c`), so the access times decide the stall.
3. **Slice ends.** The frame loop runs the 68k in slices (one per line, plus the VINT point) and
   runs the Z80 up to every slice end (only while the 68k does not hold its bus). A C function runs
   inside one slice; the Z80 run, and the stall it brings, must happen *between two of its
   instructions* (which moves the refresh phase) and not after it.
4. **Interrupts** (next section).

A function can therefore keep an exact **timeline**: it reports each original instruction, in
order, with its 68000 cost (the m68kops table: `move.w #imm,(abs).l` 20, `move.l (abs).l,Dn` 20,
`bcc` 10 taken / 8 not taken (`bcc.w` 12 not taken), `dbra` 10 / 14, `movem.l (An)+` 12+8n,
`move.w #imm,sr` 16, ...), and the layer does the rest:

    hle_insn(ctx, n)    an instruction of n cycles with no timing-sensitive access
    HLE_SEQ(ctx, ...)   a straight run of them (constants only): HLE_SEQ(ctx, 16, 8, 10)
    hle_movem_l(ctx, n, regs)   movem.l (the refresh skip above); hle_movem_w is plain hle_insn
    hle_pre(ctx)        before an instruction that touches I/O (Z80 bus, pad ports, VDP): the
                        pending time is put on the CPU clock, so bus_r8/bus_w16 happen at the
                        right time ...
    hle_post(ctx, n)    ... and then the instruction's n cycles
    hle_sync(ctx)       before an `hle_call68k` (after the `jsr` was charged with hle_insn): the
                        callee starts at the original's time

`ctx->cycles` is ignored once a function has used the timeline; the layer charges the part not
yet on the clock. `hle_insn` does the refresh check the original does at that instruction
(`tl_refresh`) and processes a slice end (Z80 run, `tl_boundary`) when the timeline crosses one;
nested 68k stepping does the same after each instruction (`nest_step_done`). Rules: charge the
`rts` too (the layer's own `rts` is free); charge a `jsr` in the caller and the callee's `rts` in
the callee (C-to-C calls are direct); list `cmp.w` + `bne` as two instructions (each starts with
its own refresh check). Nested 68k calls keep real time (`sess_end(exact)` keeps the refresh
phase they ended with); `PalFade` also opens an exact session (`hle_exact_session`).

`port/tools/seqcmp.py FUNC [--state S] [--frames N]` verifies a timeline: it runs the original
(`T2_HLE_ORIG=FUNC`) and the C replacement with `T2_HLE_SEQ=1` (one log line per call with the
cost of every instruction) and diffs them call by call; `0 differ` is exact. Two artifacts are
expected: a 10-11 cycle Z80 stall on the instruction that requests the bus in `ReadPad`, and
2-cycle noise on a call that straddles the frame wrap.

## Interrupts inside a replaced function

The original can be interrupted between any two instructions; a C function is atomic. The frame
loop raises an interrupt at a slice end (VINT at `vint_cycle` of the first line; the Menacer's
external interrupt at the start of the gun's line, `lightgun_refresh`; HINT is off in this game)
and the 68k takes it at the first instruction boundary after that. If such a slice end falls
*inside* a call, the replacement cannot reproduce the order (the handler may clear the sprite queue
between two of its stores, read the HV counter, ...). So `run_def` runs that call as the original
68k: it returns -1 and the CPU executes the instruction at the entry address normally (only entry
addresses are hooked, so the rest is plain 68k code). The same applies to a call that would
straddle the frame wrap (the frame loop rebases the clocks there).

* A function that touches only work RAM and its registers is declared `HLE_FUNC_PURE` /
  `HLE_RANGE_PURE`. The layer runs it once dry (no clock, no Z80, RAM restored) to learn its real
  length and declines if a deliverable interrupt is raised in the span (with a margin for the Z80
  stalls). No hand-made bound is needed.
* The others (I/O, or nested 68k) are judged by their `HLE_FUNC` cycle argument, which for them is
  the longest span the call can take (`ctx->cycles` is ignored): `ReadPad1/2` 330, `ReadInput`
  1713, `ReadInputAim` 2070, `ShotHitTest` 8000, `PickupHitBox` 6000, `PalFade` 14000 (its nested
  `CramUpload` polls the VDP and can take much longer than its usual ~4900).
* `HLE_FUNC_LOWMASK` (`ReadInput`, `ReadInputAim`): the function lowers the interrupt mask itself
  (`ReadPad` ends with `move.w #$2000,sr`), so an interrupt that is pending or raised during the
  call is taken in the middle of it even when it was entered inside the VBlank handler (mask 6);
  such a call also declines when one is already pending.
* Entered with the mask at 6 or 7, nothing can be delivered, so nothing is checked.

`T2_HLE_DECLINES=1` prints each decline and the total; `T2_HLE_NODECLINE=1` turns the fallback off
(seqcmp does, to get comparable runs). The difftest sandbox never declines.

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
| $011D32 | VdpListRun | game/vdplist.c | 200/200 states equal (runs the whole list; a deliberate command-6 row stride $80->$40 is caught by 1/1 and by VdpListRun 2/3); lockstep: 3000 frames identical (6 default states) |
| $011D64 | VdpCmd0End | game/vdplist.c | 200/200 states equal; lockstep: 3000 frames identical (6 default states) |
| $012068 | VdpCmd1CopyWords | game/vdplist.c | 200/200 states equal; lockstep: 3000 frames identical (6 default states) |
| $011D70 | VdpCmd2FillZero | game/vdplist.c | 200/200 states equal (calls the 68k VramFillWords); lockstep: 3000 frames identical (6 default states) |
| $011D90 | VdpCmd3WriteWord | game/vdplist.c | 200/200 states equal (calls the 68k VramWriteWord); lockstep: 3000 frames identical (6 default states) |
| $011D9E | VdpCmd4CramLine | game/vdplist.c | 9/9 states equal (rare: only the tally screens queue it; a deliberate wrong loop count is caught); lockstep: 3000 frames identical (6 default states) |
| $011D80 | VdpCmd5FillValue | game/vdplist.c | 200/200 states equal (unreachable in play, so `--entry` states captured at command 2); lockstep: 3000 frames identical (6 default states) |
| $0120BA | VdpCmd6TileRect | game/vdplist.c | 200/200 states equal (the row-stride bug above is caught); lockstep: 3000 frames identical (6 default states) |
| $011E48 | VdpCmd7HScroll | game/vdplist.c | 200/200 states equal; lockstep: 3000 frames identical (6 default states) |
| $011F6E | VdpCmd8VScroll | game/vdplist.c | 200/200 states equal; lockstep: 3000 frames identical (6 default states) |
| $005940 | HitBox | game/hittest.c | 600/600 states equal (21 hit / 579 miss; a deliberate box edge `<`->`<=` is caught at state 67) |
| $007086 | PickupHitBox | game/hittest.c | 687/687 states equal (2 hit / 685 miss; a deliberate box edge `<`->`<=` is caught at state 407) |
| $005892 | ShotHitTest | game/hittest.c | 200/200 states equal (no-hit 69 / hit 131, multi-hit sort 27, kill 55, front-not-shootable 7, human objects 4; a deliberate descending depth sort is caught at state 15) |
| $004752 | ScrollRows | game/scroll.c | 200/200 states equal (pure, per-instruction timeline, 3784 cycles avg); lockstep: 3000 frames identical (6 default states) |
| $00498A | BgStreamLayers | game/scroll.c | 200/200 states equal (nested 68k VdpQueue*/sub_013470/sub_01385A via hle_call68k, all flags mirrored); lockstep: 3000 frames identical (6 default states) |
| $009358 | ScriptTick | game/levelscript.c | 200/200 states equal (HLE_RANGE_PURE; the opcode handler stays 68k via the original `jsr (a1)`); lockstep: 3000 frames identical (6 default states) |
| $009376 | ScriptExec | game/levelscript.c | 68/68 states equal; lockstep: 3000 frames identical (6 default states) |
| $00933E | ScriptNext | game/levelscript.c | 70/70 states equal; lockstep: 3000 frames identical (6 default states) |
| $009326 | ScriptStart | game/levelscript.c | 2/2 states equal (level start is rare); lockstep: 3000 frames identical (6 default states) |
| $00B398 | ObjRunAll | game/objects.c | 200/200 states equal (HLE_RANGE_PURE, stops before `jmp (a0)` $00B3B6 / `rts` $00B3C6; the AI states stay 68k); lockstep: 3000 frames identical (6 default states) |
| $00B3BE | ObjNext | game/objects.c | 200/200 states equal (same exits; AI's `bra ObjNext` re-enters C); lockstep: 3000 frames identical |
| $00B4A6 | ObjMoveX | game/objects.c | 200/200 states equal; seqcmp 0 differ; lockstep: 3000 frames identical |
| $012316 | ObjScrollX | game/objects.c | 200/200 states equal; seqcmp 0 differ; lockstep: 3000 frames identical |
| $012332 / $012342 | ObjKillIfLeftOf / RightOf | game/objects.c | 200/200 states equal each; lockstep: 3000 frames identical |
| $00930E | ObjAlloc | game/objects.c | 200/200 states equal (a6 = $FFFF87B2: `lea.l (abs).l` sign-extends); seqcmp 0 differ; lockstep: 3000 frames identical |
| $0092DC | ObjSpawnTemplate | game/objects.c | 65/65 states equal (calls the C ObjAlloc directly; `bset #15,Dn` 10, `move.l (a1)+,(d16,a6)` 24); seqcmp 0 differ; lockstep: 3000 frames identical |
| $0099AA, $0099B0, $0099BC, $00A130, $00A138, $009C36, $00B2E6, $00B2F2 | ScrGoto, ScrScrollTab4A88/4A94, ScrPokeWord, ScrPokeLong, ScrEndLevel, ScrGosub, ScrReturn | game/scriptops.c | difftest equal on every captured call (3/3, 2/2, 2/2, 1/1, 2/2, 4/4, 4/4; ScrEndLevel not on the capture route: 2 `--entry` states from ScrPokeLong equal); HLE_FUNC_PURE with exact per-instruction timelines; lockstep: 3000 frames identical (6 default states) |
| $009C18, $009C30 | ScrText, ScrTextClear | game/scriptops.c | 2/2 states equal each (jsr wrappers, callee via `hle_call68k`; decline bound 60000); lockstep: 3000 frames identical (6 default states) |

`ScrollRows`/`BgStreamLayers` are in `game/scroll.c` (docs/subsystems/vdp.md, "Rows" and "Map columns"). The per-level
wrappers `BgStreamL1/2/4/5/6` stay 68k and enter `BgStreamLayers` by `bsr`/fall-through; the recursion
`bra BgStreamLayers` is a loop in C, and the register save/restore (`movem.l`) is written to the stack
as in the original.

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
colours (original min/avg/max 3454/10626/15466 over the 200 states), so it is charged per
instruction on the instruction timeline (every colour entry, each channel's branch path, the
`dbra`; the early exit is 62); `seqcmp.py PalFadeStep` shows 0 differing calls. (The old fit
`3454 + 172*d1 + 12*ch` was ~25 cycles off, which broke the gate on `title`.)

`Copy32` ($008A7C) is eight `move.l (a0)+,(a1)+`; the C version reproduces the 8th move's
N/Z (`move.l` sets N/Z, V=C=0) and charges 8 x 20 + the `rts` 16 = 176 on the timeline.

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
is run through `hle_call68k`; the `lea`/`jsr` that precede it are charged and put on the clock
(`hle_sync`) *before* the call, so `CramUpload` polls the VDP at the original's time (the old
lump charge ran it ~32 cycles early). The range opens an exact session (`hle_exact_session`).
Its declared window is 14000 cycles (see "Interrupts inside a replaced function").

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

## The VDP command list (`game/vdplist.c`)

`VdpListRun` ($011D32, `src/t2.s`) is the interpreter of the RAM command list that the frame
builds during gameplay and runs right after the VBlank wait (`docs/subsystems/vdp.md`). It sets
`a6 = vdp_list` ($FFB252) and then loops: `move.w (a6)+,d0; asl.w #2,d0; jmp VdpListJump(pc,d0.w)`.
The nine handlers read their arguments from `(a6)+` and either tail-`bra.w` back to `VdpListNext`
($011D38) or, for command 0, rewind `vdp_list_ptr` ($FF80BC) and `rts`. The C `VdpListRun` is an
`HLE_FUNC` that runs the whole list, calling the shared handler bodies directly; each handler is
also registered on its own: command 0 as an `HLE_FUNC` (it ends in the `rts`), 1..8 as
`HLE_RANGE`s whose single exit is `VdpListNext` (the handlers never touch the stack). Enabling a
handler alone still works: the 68k `VdpListRun` jumps to it, the hook runs the C range and it
`hle_jump()`s back to `VdpListNext`.

Commands 2/5 (`VramFillWords` $008782) and 3 (`VramWriteWord` $0087C4) are not ported, so the C
bodies reach them through `hle_call68k` (the nested 68k is exactly the original `jsr`); the other
commands' VDP writes are inline and reproduced with `vdp_ctrl_w`/`vdp_data_w` in the original order
and width (status poll, `bset`/`bclr` of the DMA-enable shadow bit `$FFA0DD` bit 4 around the copy
where the original does it, autoincrement `$8F02`/`$8F20`, the 14-bit address + CD bits, and for
command 4 the 32-bit CRAM command). Every register write mirrors its width (`move.w` = `set_w`,
`moveq`/`lea`/`movea` = full) and the final CCR of the last flag-setting instruction (command 6:
the closing `addi.w #$80,d3`; command 4/8: the closing `move.w`; command 7: the last data write).

**The status poll is timing-sensitive.** The handlers' `move.w (4,a5),d0` reads the VDP status and
its value ends up in d0/d1/d2, so it must be read at the same raster time as the original. From C
the CPU clock is frozen, so the prologue (and, for command 4, each per-iteration poll) is run as
68k with `hle_run68k_until` up to the read, then the C continues; `hle_call68k` also skips the
`jsr`'s 20 cycles, so `jsr_tick()` adds them before the call. A 68k control-port word read also ORs
in bits 10-15 of the prefetched instruction word (`mem68k.c` `vdp_read_word`), and from C
`m68k.pc` is the read instruction itself, so `vdp_status_word()` rebuilds that part from
`ROM16(read_addr+4)`.

**Sandbox determinism.** `sb_begin()` (port/hle/sandbox.c) now pins the VDP raster line base to
the restored CPU clock and clamps the FIFO read-out stamps (`gx_vdp_fifo_clamp`) to it. The
savestate does not carry either, so before this a status read's FIFO/VBLANK/VINT bits leaked in
from whatever ran before (the reference side or the previous state) and differed between the
reference and the C side. This is a difftest-only change (the sandbox is not used by `t2port`); the
already-ported functions still pass.

Cycles: each translated instruction advances the real CPU clock as it executes (next section).
Commands 2/3/5's nested VramFillWords/VramWriteWord run as real 68k. `T2_VDP_STATS=1` reports the
per-command call counts of a run (used to show every command is exercised by the captures).
Captures
(`port/scripts/capture_vdplist.txt`, level loads + mouse play, and `capture_bcdadd.txt`, the tally
screens). Command 4 (CRAM line) is rare — the
tally screens queue it 9 times over all routes — and command 5 is never queued by the shipped game
(its queuers `$00BE94`/`$00BF1E`/`$00C20A` are unreachable), so its states are captured at command
2 and replayed with `--entry`:

    build/port/difftest capture-multi 200 v 011D32 011D64 012068 011D70 011D80 011D90 011D9E \
        0120BA 011E48 011F6E --script port/scripts/capture_vdplist.txt \
        --script port/scripts/capture_bcdadd.txt --out runs/vdp
    build/port/difftest capture-multi 100 s 011D9E --script port/scripts/capture_bcdadd.txt \
        --script port/scripts/capture.txt --script port/scripts/capture_score.txt \
        --script port/scripts/capture_score2p.txt --out runs/vdp_end   # the 9 command-4 states
    port/tools/dt VdpListRun VdpCmd0End VdpCmd1CopyWords VdpCmd2FillZero VdpCmd3WriteWord \
        VdpCmd4CramLine VdpCmd6TileRect VdpCmd7HScroll VdpCmd8VScroll
    build/port/difftest run VdpCmd5FillValue --states runs/vdp/011D70_v --entry   # cmd 5

`port/tools/mousetest.sh runs/t2port-hle-all.sh` (the wrapper enables `--hle all`) passes flow,
twop, continue, missile, grid and lag.

### Exact timing (lockstep-identical)

A lump cycle charge cannot be exact here. During active display every VDP data-port write can
stall the CPU (GPGX `vdp_68k_data_w_m5`: FIFO full / access slots, ~60 cycles per VRAM word, the
cost of a copy depends on the clock at each write), and the frame loop does work between lines
that a running C function never sees. The original's cost of a `VdpCmd7HScroll` call is ~3008
cycles at a level load; the old lump charge was 2204. What makes the C version identical:

- **Real clock.** Every translated instruction advances the CPU clock (`pre()` / `post()` /
  `tick()` / `wr_data()` in vdplist.c) in the core's order: line step, bus refresh check
  (`refresh_cycles`, +2 cycles every 128), the access at that clock, then the instruction's
  cost. Runs of several instructions are ticked one instruction at a time (`TICKS`), because the
  refresh check belongs before each one. The functions open an *exact session* (`sess_begin`:
  `hle_dma_begin` + `hle_exact_session`): hle.c then keeps the refresh phase and the FIFO
  read-out stamps, returns the elapsed time (stalls and refresh included) as the charge together
  with its sub-cycle remainder (a stall ends on a 7-mclk boundary), and skips its refresh model.
- **Timed data writes.** `wr_data()` brackets the write with `hle_vdp_timed(1)`, so `bus_w16`
  does not take the "untimed" path of gx.c and GPGX applies its FIFO stall to the clock. The VDP
  line base and V counter follow the clock (`gx_vdp_follow_cpu_lines`) as in a nested call.
- **The Z80 per line.** In an exact session the line step also runs the Z80 (`gx_follow_z80`,
  like the frame loop after each `m68k_run` slice): a Z80 read of the 68k bus (the sound driver
  streaming samples from the ROM bank) delays the 68k by ~10 cycles, so a 68k that crosses a line
  start must see those delays where the real CPU does. This was the last big source of drift
  (a 70-mclk step at a line start).
- **The timing guard (`safe_run`, `gx_vdp_next_toggle_clock`).** The frame loop also changes
  state at some line starts, and interrupts arrive in the middle of a command. A command that
  might cross one is left to the real 68k: a range handler calls `hle_decline()` (the CPU runs the
  instruction at the entry normally; ignored in the sandbox), and the C `VdpListRun` hands the
  rest of the list to the 68k at a command boundary (`hle_jump` to `VdpListNext` with all
  registers mirrored; the handlers then decide for themselves). The events: VBLANK set (line
  `viewport.h`; the V-Int rises just after the line starts, so that line is never safe), VBLANK
  cleared (last line) and the frame wrap (FIFO reset, clock rebase), the light gun's line and the
  next one (HV latch, external interrupt), every line when H-Int is on, and a DMA in flight (the
  frame loop halts the CPU for it per line). Whether a command is "safe" is only an upper bound
  on its duration (`cmd_bound`: ~72 cycles per VDP word, 110 for VramFillWords, plus a one line
  margin) against the next such line, so a loose bound only means the 68k runs the command; it
  can never make the result differ. Typical outcome: the short commands in the middle of a frame
  run as C; the long level-load copies that span VBlank stay 68k.

**Result.** `port/tools/lockstep.sh --hle VdpListRun,VdpCmd0End,VdpCmd1CopyWords,VdpCmd2FillZero,
VdpCmd3WriteWord,VdpCmd4CramLine,VdpCmd5FillValue,VdpCmd6TileRect,VdpCmd7HScroll,VdpCmd8VScroll`
is **3000 frames identical** from `gun_01 gun_02 gun_04 gun_05 gun_07 title` (before: diverged at
frames 20-22 on the level load; first with the clock model only 23-81, then 34/842/1482... until
the Z80, V-Int and refresh-order fixes above). Each handler alone is also identical. difftest
(the sandbox never declines) is still all-equal for every VDP function.

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
Play-test: `port/tools/mousetest.sh runs/t2port-hle-all.sh` (a wrapper that runs `t2port --hle all`)
passes all six tests (flow, twop, continue, missile, grid, lag). Alone, the three hit-test
functions pass the lockstep gate far longer than the VDP list: `PickupHitBox` is identical for
3000 frames from every default state, `ShotHitTest`/`HitBox` still drift late (frames 846-2545 on
`gun_02`/`gun_05`).
