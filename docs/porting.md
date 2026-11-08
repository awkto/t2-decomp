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


`T2_HLE_PROFILE=1` measures how much of the game still runs as 68k (zero cost when unset): the CPU exec
hook charges the master clocks between instructions to the containing function of `data/functions.txt`
(run from the repo root), the cycles a C call charges count as "C" (68k run nested inside it, apart), the
waits (WaitVBlank, FramePaceWait's loop, the $01DC4E VDP wait) as "idle", and a summary with the top 40
functions is printed at exit. `port/tools/profmerge.py LOG...` merges several runs' `PROF` lines. Replaced
routines that the layer declined (an interrupt falls inside) show with their decline count. Results:
`docs/remaining.md`.

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

**Cold-boot gate.** Every other gate starts from a savestate or a route taken after boot, so the
power-on path (RamClear, logos, story, title, the first attract demo) needs its own run:

    python3 -I port/tools/scriptgate.py port/scripts/coldboot_gate.txt --hle all   # 9000 frames identical

The script is `run 9000` from power-on with no input (~25 s on the VM; `T2_HASH_Z80=1` adds the sound
state: also identical). Caution when reading `--framehash` files by hand: column 2 is the hash of the
*full* work RAM including the dead system stack, and it differs between `--hle none` and `--hle all`
at a few frames (cold boot: frames 241, 247, ...; one dead-stack byte such as `$FFFF93`) even though
nothing the gate compares differs. Compare the combined column (ALL) or use `lockstep.py`/`scriptgate.py`,
which do; a bare `cmp` of two hash files reports false divergences. `--hle none` is deterministic
(two cold boots give identical hash files).

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
  1713, `ReadInputAim` 2070, `ShotHitTest` 24000, `PickupHitBox` 6000, `PalFade` 14000 (its nested
  `CramUpload` polls the VDP and can take much longer than its usual ~4900).
* `HLE_FUNC_LOWMASK` (`ReadInput`, `ReadInputAim`): the function lowers the interrupt mask itself
  (`ReadPad` ends with `move.w #$2000,sr`), so an interrupt that is pending or raised during the
  call is taken in the middle of it even when it was entered inside the VBlank handler (mask 6);
  such a call also declines when one is already pending.
* Entered with the mask at 6 or 7, nothing can be delivered, so nothing is checked.

`T2_HLE_BOUNDS=1` is the check mode for those hand-made bounds. For every executed top-level
non-pure call it measures the real span from entry to return (C plus nested 68k) and, at exit,
prints per function: the declared bound, the max observed, the call count, the worst
observed/bound ratio, and `OVER` when a call exceeded its bound (`near` above 80%). The VDP list
commands declare their bound through `safe_run(n)` (`vdplist.c`); each noted bound is checked
against the span up to the next note or the end of the call. Zero cost when the variable is unset.
`port/tools/bounds.py` runs `--hle all` with it from the savestates (lockstep.py's input script)
and merges the tables; re-run it after changing a bound or adding a non-pure replacement, and
raise any bound that is OVER or near (observed max x 1.5, with the observed value in a comment).

`T2_HLE_DECLINES=1` prints each decline and, at exit, a table of declines per function and reason (frame end, interrupt window, pending irq; for the VDP list: DMA, V-Int line, VBLANK-clear/wrap, H-Int, light gun; `port/tools/declines.py` runs it over several states together with `T2_HLE_PROFILE`); `T2_HLE_NODECLINE=1` turns the fallback off
(seqcmp does, to get comparable runs). The difftest sandbox never declines.

## Early bail and resume (HLE_F_BAIL)

Declining a call whose worst-case window holds an interrupt (or the frame end) hands the *whole* call to the 68k,
which is expensive for long functions whose usual path is short, and for long loops. `HLE_F_BAIL`
(`HLE_FUNC_BAIL`, `hle_bail_due`, `hle_bail_to` in hle.h) lets the C run up to the last place that is certain to
come before the event and hand over there; the 68k then runs the stretch that holds the interrupt exactly as the
original does.

* `run_def` does not decline a BAIL function. It computes `g_bail_at` = the first interrupt-raising slice end
  (`gx_irq_boundary_in`, skipping the ones the SR mask hides) or the frame end, whichever comes first. Only a
  top-level call has one (a nested call runs inside its caller's guard; the sandbox never bails).
* At an instruction boundary where every register is in ctx and the next instruction sets the flags the rest
  reads (the flags before it are dead; the Bcc/dbra/move at a loop head or a `jsr` site), the function asks
  `hle_bail_due(ctx, chunk)`; `chunk` is an upper bound (68k cycles) of the code up to the NEXT such point (or
  the `rts`). If the clock plus the pending timeline plus the chunk reaches `g_bail_at`, the function calls
  `hle_bail_to(ctx, pc)` and returns: ctx holds the registers, the 68k continues at `pc`, the layer charges the
  timeline run so far. A bail *before* the event is exact because the C part is on the instruction timeline
  (hle_insn: refresh, Z80 catch-up per slice end) and the rest is the original code; an over-large chunk only
  costs 68k time. Nothing charged yet (`ctx->tl_used == 0`) means nothing changed: decline instead (a bail to
  the address the hook is at would loop).
* Only the function the hook entered may bail. C-to-C calls share the ctx (PlayersButtons calls the C
  PlayerButtons/PlayerShoot), so those check `ctx->def->addr` first (`PL_BAIL` in player.c).
* **Resume.** A bail address (a loop head or a call site) can itself be registered as an entry of the same C
  body (`BuildSpriteTableSort/Sprite/Piece/Tail`, `ScrollRowsGroup/Row/List`, `MissilesSlot`, the VDP list
  `VdpCmd1CopyLoop`, `VdpCmd6Row/Words`, `VdpCmd7HScrollA/Setup/B`): the 68k that was handed the stretch reaches the
  next loop head, the hook enters the C body there (a `goto` by entry address), and the rest of the call runs as
  C unless an event is again close (then it declines at once: the 68k executes that one instruction and the
  hook asks again at the next head). So the 68k runs only the stretch around the event, not the tail. The VDP
  commands (vdplist.c, `resume_start`/`resume_finish`) do the same through their own exact-session guard
  (`head_ok`, `due`): a command whose head did not fit before VBLANK-clear/the wrap is finished by the 68k up to
  its loop head and taken over there.
* Users: BuildSpriteTable (sort pass / sprite / piece / tail heads; chunk 170-350), BgStreamLayers (pass top
  260, the heavy pass after the `blt`: 76500), ScrollRows, MissilesRun (slot head 120, live slot 39000),
  PlayersButtons/PlayerButtons/PlayerShoot/EnergyHud/GunpowerHud (before each `jsr`/`bsr`, chunk = the old whole-call
  bound; the callee is its own top-level hook once the 68k executes the `jsr`), VdpCmd1/6/7 loops. `T2_HLE_DECLINES`
  prints `bails:` lines per function.
* Gates: the difftest sandbox does not bail (it checks the full path, `dt` per entry), so the proof for the bail
  paths is the lockstep gate (`--hle all`, six states, then all 92 states, with `--z80`) and the route gates.

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
| $00B586, $00B6D0, $00B5EC, $00B65A, $00B71E, $00C564, $00C5E2 | EndoWalk, EndoWalk2, EndoShoot, EndoAim, EndoShoot2, EndoWalkAnim, EndoStand | game/ai_endo.c | type-$01 AI states, HLE_RANGE_PURE to `bra ObjNext`; 200/200, 140/140, 200/200, 200/200, 200/200 (`--entry` from the walk states), 200/200, 24/24 states equal; lockstep: 3000 frames identical (6 default states, also `--hle all`) |
| $00B646, $00B75E | EndoShootAfterSound, EndoShoot2AfterSound | game/ai_endo.c | the part of $B5EC/$B71E after the real `jsr sub_013D04` + `bsr sub_009054` (the first ranges exit before the jsr); 200/200 each |
| $00B52E, $00B504, $00B520 | EndoDeathFlash, EndoDeath, EndoDeathAfter | game/ai_endo.c | death flash; $B504 exits before the real `jsr sub_01277C`, $B520 resumes; 200/200 each (`--entry`) |
| $00C66C, $00C6B0, $00C7C8, $00B4C2, $00B78C | EndoDown, EndoFall, EndoFall2, EndoHitDown, EndoHitDown2 | game/ai_endo.c | knocked-down/falling states (sub_00D236 and the VDP-list copy inlined; $C66C falls into $C6B0, $C6B0 into $C7C8); 200/200 each (`--entry`) |
| $005A48, $0059AC, $005AF8, $005BEC | EndoHit, EndoHitHead, EndoHit3, EndoHit2 | game/ai_endo.c | hit routines (HLE_FUNC_PURE, call the C HitBox); 76/76, 76/76, 76/76 (`--entry` from 005A48), 63/63 |
| $00EA26, $00EA6A, $00EAEC, $00EB2E, $00EB96, $00EC16, $00EA54, $00EA92, $00EB4C, $00EB62, $00EBBC, $00EAD6, $00EB9E | L2Crate, L2Lamp, L2Lamp2, L2Crate2, L2Crate3, L2Tail, L2St_EA54/EA92/EB4C/EB62/EBBC, L2Human_EAD6, L2Crate4 | game/ai_l2.c | level-2 states of types $0F/$20/$13 (HLE_RANGE_PURE to `bra ObjNext`; `capture_l2.txt` route from gun_02/pad_02/2p_gun_02): 300/300 each (222/222 EA54, 172/172 EB9E, 100/100 EAD6 by `--entry`); seqcmp 0 differ where called |
| $00EAA8, $00EAF4, $0151F0, $0151EA, $015212, $01522A, $015208 | L2Human, L2HumanFlinch, L2Lamp3, L2Lamp3b, L2Human15, L2Human15b, L2Human15kill | game/ai_l2.c | type $13 (level 2) and $20 states; 300/300, 60/60 x4 (`--entry`, timer pokes), 300/300, 60/60, 300/300, 60/60, 60/60; seqcmp 0 differ where called |
| $00EA2E/$EA42/$EA4C, $00EA72/$EA8A, $00EB36/$EB44, $00EB78/$EB8C, $00EBD2/$EBDE, $0151CC/$0151E2 | L2Kill_* | game/ai_l2.c | kill starts of the crate/lamp: the range stops before each real debris-spawner `jsr` (sub_01253C, sub_012A9C, ...) and a second range at the return address continues; $EB78 pushes a6 around the call (`HLE_EXIT_SP` -4 / +4); 1-4 real states each + 60 `--entry` states each, all equal |
| $00EFA4, $00F18A, $00EDD4, $00EFFE/$F030, $00F1E4/$F216, $00EE2E/$EE60 | L2InfWalk(2,3), L2InfStand(,2,3) + *Snd | game/ai_l2.c | type $26 infiltrator walk and stand/shoot states (three variants); the warning sound + damage `jsr`s stay 68k between the two ranges; 300/300 each; seqcmp (NODECLINE) 0 differ for the Snd ranges and EE2E, but the walk states and EFFE/F1E4 diverge from call ~2300 on (believed an interrupt-straddle artifact of NODECLINE, not proven; lockstep with declines is identical) |
| $00F69C, $00F6CA, $00F6F4, $00F722 | L2InfDrop, L2InfLand, L2InfDrop2, L2InfLand2 | game/ai_l2.c | type $26 ceiling drop and landing; 264/264, 180/180, 189/189, 135/135; seqcmp 0 differ |
| $00EED4, $00EF0A, $00EEB0, $00F094, $00F0CA, $00F066, $00F27A, $00F2B0, $00F24C | L2InfDie(,2,3), L2InfFall(,2,3), L2InfLast(,2,3) | game/ai_l2.c | type $26 death sequences (init, fall via sub_00D236, VDP-list tile copy, fade out); 1/1, 33/33, 64/64, 3/3, 99/99, 192/192, 1/1, 33/33, 25/25 |
| $00597A, $006512, $006544, $006576, $0065A4, $0065D2, $0066A8, $00683A, $006894, $0068CA, $0065FC, $006652 | L2HitNone, L2Hit_*, L2HitHumanB/C | game/ai_l2.c | +$1C hit tests of the level-2 $0F/$20/$26/$13 objects (call the C HitBox); 226/226 and 300/300 (176/176, 242/242, 99/99 where fewer calls); seqcmp 0 differ; lockstep (`--hle` the 67 L2 entries, then `--hle all`): 3000 frames identical (6 default states) |
| $00DAC4, $00DB8C, $00EC58, $00EC82, $00ECB2, $00ECE8, $00ED2E, $00ED5C | L4Box, L4Lamp, L4PopRise, L4PopShoot, L4PopShootAfterSound, L4PopSink, L4PopHit, L4PopKill | game/ai_l4.c | level-4 states of types $1F, $21, $29 (HLE_RANGE_PURE to `bra ObjNext`; $EC82 stops before the sound `jsr`, $ED5C is a plain HLE_RANGE with nested 68k, bound 17500 = observed max 11416 x 1.5): 200/200, 200/200, 200/200, 200/200, 200/200, 176/176, 40/40, 2/2 (`capture_l4.txt`) |
| $006326, $00638E, $0067F2 | L4HitBox, L4HitLamp, L4HitPop | game/ai_l4.c | +$1C hit tests of $1F/$21/$29 (call the C HitBox); 2/2, 12/12, 6/6. Lockstep: the 11 entries 3000 frames identical on the six default states, `--hle all` identical, and `--hle all` identical on 45 states (/opt/t2/vstates). |
| $00DAEE, $00DB0C, $00DBC4, $00DBE0, $00E166, $00DBF6..$00DD96 (panels), $00F540..$00F65A + $00DDBA..$00E0A4 (orbs), $00E174, $00E1CA | L4BoxFlash, L4BoxKill, L4LampKill, L4LampDead, L4Mover, L4Panel* (9), L4Orb* (17), L4MoverKill1/2 | game/ai_l4.c | the rest of level 4 (box/lamp kill states, $24 movers, $22 wall panels, $23 orbs; were under `L4_UNVERIFIED`, now registered): the 36 entries 1-200 states each (the kill states 1-4: once per object), plus hit tests $006424/$006456/$006488/$0067C6/$006900 (200/200, 47/47, 164/164, 200/200, 200/200). Bugs fixed: the path stepper read the ROM path data from RAM. Bounds from T2_HLE_BOUNDS/dt maxima x 1.5: BoxKill 1200, LampKill 1900, OrbKill 9300, OrbKill2 10600, MoverKill1 7900, MoverKill2 14500. States `l4_*` from the gun-bot (tools/harness/scripts/l4, `capture_l4b.txt`/`capture_l4c.txt`). seqcmp: 0 differ except Random's `movem.l d0-d2,-(sp)` logging 30 vs 32 when the refresh skip fires (log artifact: hle_insn_x logs n; lockstep is identical). Lockstep: identical on the six default states, `--hle all`, and all states in /opt/t2/vstates |
| $00765E..$0078C2 (20 pieces), $008B3C | GameLoop, GameLoopAfterPal, GameLoopPauseCheck, GameLoopPause{Demo,Hud,Rel,Wait,End}, GameLoopSfx, GameLoopGunpower, GameLoopHudDone, GameLoopBg1/2/4/5/6, FramePaceCheck/Skip/Done, GameLoopEnd, ClearObjectPools | game/gameloop.c | the per-frame driver, generated instruction by instruction (`costcheck.py`: 120 charges, 0 bad). GameLoop is never `jsr`ed and every call in it must keep its own dispatch/interrupt timing, so it is HLE_RANGE_PURE pieces that cover only its OWN instructions, each from the loop top or a `jsr` return address up to the next `jsr` (exit = that instruction, which the 68k executes), LevelEnd ($0078D0) or the loop top; the pause loop is the same around its WaitVBlank/ReadInput/sound calls. FramePaceWait's wait (`cmp.w ; bgt.s` at $0078A6, and the sign-wrap `jsr WaitVBlank`) stays 68k: FramePaceCheck stops before it, FramePaceSkip/FramePaceDone resume after it (no catch-up, VdpListRun after the wait unchanged). Pure, so no hand bounds (the layer declines a piece when an interrupt falls inside). ClearObjectPools is one HLE_FUNC_PURE (~128900 cycles: it declines when the frame end or an interrupt falls inside, but is exact when it runs). difftest: GameLoop/AfterPal/PauseCheck/Sfx/Bg1/2/4/5/6/HudDone/FramePaceCheck/Done/GameLoopEnd 200/200, Gunpower 14/14, PauseWait 200/200, PauseDemo 5/5, PauseHud 4/4, PauseRel 4/4, PauseEnd 4/4, FramePaceSkip 3/3 (`capture_pause.txt`: pause, unpause, B+C cheat, demo interrupt, vblank sign wrap), ClearObjectPools 2/2. `port/scripts/pause_gate.txt` (+ `port/tools/scriptgate.py`: Start is never pressed by the lockstep input, so this route pauses/unpauses 4 times, uses the B+C cheat, interrupts the demo and pauses in a 2P state): 5738 frames identical `--hle all`. Dispatch counts (92 states x 3000 frames, `--hle all`, T2_HLE_TRACE=1): GameLoop 254483, GameLoopHudDone 254481, FramePaceCheck 254480, FramePaceDone 254448, GameLoopEnd 254448, GameLoopBg1 254480, Bg2 65196, Bg5 80557, Bg6 12364, Bg4 12037, GameLoopAfterPal 15719, PauseRel 2952, PauseCheck 320, Sfx 270, Gunpower 451, ClearObjectPools 38, PauseDemo 1, PauseHud 1 (PauseWait/PauseEnd/FramePaceSkip: `pause_gate.txt` and `--entry` only). Lockstep 3000 frames identical: own list and `--hle all` (six default states) and `--hle all` over all 92 states (4 groups in 4 copies on the VM); mousetest ALL PASS |
| $0074D8, $0078D0, $001D3A, $001DB4..$002980 (157 pieces) | NextLevel, LevelEnd, LevelLoadDispatch, LevelLoad1..8 and LevelFlow_xxxxxx (one per `jsr`/`bsr` return address) | game/levelflow.c | the level flow, generated instruction by instruction from the disassembly (`costcheck.py`: 631 charges, 0 bad). Every `jsr`/`bsr` in these routines is VDP work, a blocking wait (WaitVBlank, WaitFrames, PalFade), a sound request or a registered callee, so each routine is HLE_RANGE_PURE pieces that cover only its OWN instructions, from the routine start or a `jsr` return address up to the next `jsr`/`bsr`/`rts` (exit = that instruction, which the 68k executes). All pieces share one function (`lf_run`, a switch on the entry address, branches are gotos). No VDP port is touched in C, so no hand bounds (the layer declines a piece when an interrupt falls inside). LevelLoad1/2/3/4/5/6/7/8 ($002980 $00241E $00281E $002632 $00229E $002154 $001FB2 $001DB4) start with a `jsr`, so their first piece is the return address of that call. difftest: all 157 entries equal (824 states from one `capture-multi` pass over `port/scripts/levelflow_gate.txt`, up to 40 calls each; a deliberate +4 on one `clr.w` is caught by the gate at frame 6517). `port/scripts/levelflow_gate.txt` (+ `scriptgate.py`): ends levels 1..8 with Menacer (`level_exit` poke), 1P joypad (the real pause + B+C cheat), 2P Menacer+pad and 2P two pads, the level 8 ending, the high-score initials screen, game over to initials by mouse, death with credits and the continue screen, and the attract demo (Start interrupt, a demo level end, the level 8 -> 1 wrap): 43913 frames identical `--hle all`. Dispatch counts (92 states x 3000 frames, `--hle all`, T2_HLE_TRACE=1): 154 of the 157 entries run (NextLevel 1, LevelEnd 8, LevelLoadDispatch 3, LevelFlow_00764E 42, loaders 4-5 each; the level-flow route covers the rest). Lockstep 3000 frames identical: own list, `--hle all` (six default states), and `--hle all` over all 92 states (4 groups in 4 copies on the VM); pause_gate.txt identical; mousetest ALL PASS |
| $000900..$001BB0, $003EC8, $003EFE, $003F98..$004068, $0041E6..$0045EA, $004650, $004668, $006F64/$006F78/$006F8C, $008D68, $008DFE, $01524A..$0152A2 (228 pieces) | ColdStart, CheatCodeCheck, WaitStartWindow, MainMenuLoop, MainMenuInput, OptionsInput, Ending, TitleScreen, PressStartLoop, HiScoreInitOnce, SortHiScores, StoryIntro, PlayerDied, ContinueCountdown, InitialsAI/InitialsHit1/2/InitialsOnHit (hi-score initials) and Screens_xxxxxx (one per `jsr`/`bsr` return address) | game/screens.c | the front-end screens (logos, story, title, main menu, options, ending, hi-score table, continue countdown, initials entry), generated instruction by instruction like levelflow.c as HLE_RANGE_PURE pieces between the jsr's (`costcheck.py`: 921 charges, 0 bad); stack-pushing pieces declare HLE_EXIT_SP exits. Barriers left to the 68k: every jsr/bsr/jmp/rts, `mulu` ($0014D4, $001B02) and the story screen's VDP write through a5 ($004320). WaitVBlank callers keep their addresses (mouse.c screen detection unchanged). difftest: 228/228 pieces equal (capture-multi over `port/scripts/screens_gate.txt`, up to 40 states each; difftest takes <=64 addresses per pass); PressStartLoop is only reached by a goto from TitleScreen. `port/scripts/screens_gate.txt` (+ `scriptgate.py`): power-on logos (Start before/inside the window), story, title with the cheat code, menu rows, every option row changed, 1P Menacer/joypad, 2P Menacer+pad and two pads, continue taken and expiring, initials by mouse and by joypad crosshair (P1 and P2), the idle logo/story/title/attract/hi-score loop, the ending: 34882 frames identical `--hle all`; pause_gate and levelflow_gate identical |

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

Commands 2/5 (`VramFillWords` $008782) are not ported and command 3's `VramWriteWord` $0087C4 is C in
`game/textprint.c`, so the C bodies reach them through `hle_call68k` (exactly the original `jsr`; the nested
dispatch runs the C VramWriteWord); the other
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
Commands 2/5's nested VramFillWords runs as real 68k; command 3's VramWriteWord is the C one (textprint.c). `T2_VDP_STATS=1` reports the
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
  run as C.
- **Bail out at the event (copy words, hscroll).** The VDP list does not run right after VBlank
  starts: the game logic takes ~26 lines, so the list starts about 10 lines before the frame wrap
  and its commands cross VBLANK-clear and the wrap. Declining whole commands for that (the
  worst-case bound is ~10 lines) made 99% of the hscroll and half of the copy calls run as 68k.
  Commands 1 and 7 therefore run their loops in C instruction by instruction and, at the first
  instruction boundary at or after the event's clock (`g_tog` = `gx_vdp_next_toggle_clock()` at
  entry, checked by `due()` before each instruction, i.e. where the core's `m68k_run` slice ends),
  mirror every register and the CCR into the context and jump into the original code at that
  instruction (`hle_ctx.bail`, allowed by `run_def` whatever the declared exits). The 68k then
  finishes the command after the frame loop has done its work, as in the original. Only the
  fixed-length head (arguments, control port writes, ~300 cycles) is guarded up front
  (`head_ok`, 450 cycles, no margin). The end command (0) is guarded too: its dispatch and rewind
  must not slip across the event (an unguarded one made `vdp_list_ptr` differ for one frame).
  Commands 2-5 and 8 still need to fit whole (copy words, tile rect and hscroll bail and resume, see "Early bail" below). `T2_NOBAIL=1` restores the old all-or-nothing
  behaviour (A/B measurements).

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
| $00C886, $00C8F0, $00C91E, $00CA0A | ai_crate, crate_open2, crate_open3, ai_wall_section | game/ai_simple.c | AI states of types $0F/$14 (HLE_RANGE_PURE to `bra ObjNext`; ObjScrollX/ObjKillIfLeftOf/ObjQueueSprite/Random called as C): 300/300 and 2400/2400 (`--entry` from ai_crate states with `--poke W:@14=..`), 1200/1200 each, 300/300; per-call cycle avg equal; seqcmp 0 differ (ai_crate 2650 calls, ai_wall_section 6144); lockstep: 3000 frames identical |
| $00C8BA, $00CA12 | crate_open1, wall_kill | game/ai_simple.c | crate opening (debris spawn) and wall kill (debris + tile rect via `hle_call68k`): 1500/1500 and 1200/1200 `--entry` states with slot-relative pokes; rare on the routes, so also a poked level-1 scenario (slots set to these states at frame 2406) vs `--hle none`: all frames identical incl. cycle column |
| $00C9A2, $00C968, $00C95E | ai_human_soldier, human_flinch, human_kill | game/ai_simple.c | human soldier AI / flinch / kill (tile rect via `hle_call68k`; windows 3000): 300/300 (+1800 poked), 40/40 (+200), 300/300 `--entry`; seqcmp 0 differ (1242 and 166 calls); lockstep identical |
| $00597C, $005F04, $005F56 | hit_destructible_prop, HitHumanSoldier, hit_wall_section | game/ai_simple.c | +$1C hit tests of types $0F/$13/$14 (call the C HitBox directly): 67/67, 33/33 (+132 with shot_damage pokes), 144/144 (+1152 with +$2A pokes); seqcmp 0 differ; lockstep: 3000 frames identical |
| $00CB34, $00CB9E, $00CC98, $00CD1C, $00CD3A, $00CD4E | HkBodyLeft, HkBodyRight, HkBodyDie, HkBodyEnd, HkBodyEnd1, HkBodyEnd2 | game/ai_hktank.c | level-1 boss body ($16): approach/retreat with the tile-animation tail $00CBE8 and the spark helper $00CD66 (tile rects, sub_0081B6, sub_009A02 via `hle_call68k`; bounds 8800), death ($00CC98 + explosions $00CCE6, bound 35000), countdown ($00CD1C pure; stops before the two `jsr ScriptNext`, $00CD3A/$00CD4E continue). Boss states `hk_*` made with the gun-bot (see capture_hktank.txt, tools/harness/scripts/hk): 200/200 each natural, 480/480 and 180/180 with +$2A/+$24/X pokes (HkBodyLeft with `--ignore-regs d2` on the sub_0081B6 path: d2 is the VDP status, clock dependent in the sandbox), deaths 200/200 `--entry`; seqcmp 0..5 differ of 585-915 calls (+-2 cycles on a VDP-FIFO stall inside sub_0081B6, no drift); lockstep: identical |
| $00CDFE, $00CE0C, $00CE46, $00CEC4, $00CEFA, $00CF9E, $00CFDE | HkHull, HkHullDie, HkTurret, HkTurretDie, HkPartDying, HkArm, HkArmDie | game/ai_hktank.c | hull/turret/arm ($17-$19) AI and death states, the shared dying state $00CEFA and the turret's fire helper $00CF58 (Random C, sub_0099D4 via `hle_call68k`): 200/200, 200/200 `--entry`, 200/200 (+440 poked), 200/200 `--entry`, 158/158 (+390 poked `--entry`), 200/200, 200/200 `--entry`; seqcmp 0 differ (HkHull 1500, HkArm 3000 calls), HkTurret/HkPartDying 6/2 of 1500/39 differ by +-2 cycles; lockstep: identical |
| $00D028, $00D08C, $00D0C8 | HkWeak, HkWeakBlink, HkFree | game/ai_hktank.c | weak point / turret shot ($1A): movement script sub_00C48E inlined, blink, free; $00D08C stops before the `bsr PlayerDamageNearest`: 200/200 (+210 poked), 125/125 (+180 poked), 1/1; seqcmp 0 differ (612, 300 calls); lockstep: identical |
| $006126, $006174, $0061CA, $006208, $006004 | HitHkBody, HitHkHull, HitHkTurret, HitHkArm, HitHkWeak | game/ai_hktank.c | +$1C hit tests of types $16-$1A (call the C HitBox directly; the on-hit routines $006146.. stay 68k): 51/51, 42/42, 33/33, 39/39, 8/8 (+90/+90/+48 with parts-byte and +$14 pokes); seqcmp 0 differ (75, 75, 75, 150, 46 calls); lockstep: identical |
| $00C0AA, $00C0E8, $00C242, $00C252, $00C264, $00C332 | FlyHkIn, FlyHk, FlyHkFree, FlyHkDie, FlyHkDie2, FlyHkPlain | game/ai_l3.c | level-3 flying HK ($0E) AI: fly-in, tile-art state (two VdpQueueTileRect/Mirror via `hle_call68k`, sub_009078/sound 68k), free + art cleanup, dying counter with sparks (sub_01253C), plain sprite; 192/192, 200/200, 12/12, 200/200 and 200/200 and 200/200 (`--entry` from $00C0E8) ; lockstep: identical |
| $005CC6, $005CEE | HitFlyHkPlain, HitFlyHk | game/ai_l3.c | flying HK hit tests (call the C HitBox): 36/36 each (the first `--entry` from $005CEE); lockstep: identical |
| $00C4E4, $00C4EA, $00C54A, $006042 | BombFixed, BombAnim, BombPuff, HitBomb | game/ai_l3.c | type $09 explosion (shared blink/spark tail $00C4F4, sub_012560 68k), the puff state $00C54A and the hit test: 200/200, 200/200, 200/200 (`--entry` from $00C4EA), 36/36; lockstep: identical |
| $00D674, $00D6CA | Endo1dWalk, Endo1dFire | game/ai_l3.c | type $1D endoskeleton variant (sound sub_013D04 and PlayerDamageNearest sub_009054 via `hle_call68k`): 200/200 each; lockstep: identical |
| $00D712, $00D746, $00D800, $00D89E, $00D8DC, $00D90E, $00D93E, $00D962 | Edge1eWait, Edge1eStrips, Edge1eFlash, Edge1eBlink, Edge1eGone, Edge1eB, Edge1eC, Edge1eD | game/ai_l3.c | type $1E edge object states (sub_010666, sub_007DEE strips, VdpQueueFillRows, sounds via `hle_call68k`; bounds up to 46000): 200/200 each ($00D90E-$00D962 `--entry` from $00D89E); lockstep: identical |
| $00BD54, $00BD22, $00BE2E, $005DC0 | BgStruct, BgStructHit, BgStructFree, HitBgStruct | game/ai_l3.c | type $07 background structure: two tile rects + eight column-scroll words, ObjMoveX C; 200/200, 200/200 (`--entry` from $00BD54), 8/8, 12/12; lockstep: identical. Captures: `port/scripts/capture_l3.txt` (gun_03/2p_gun_03 with mouse shots) |
| $00F8A2, $00F8EA, $00FB52/$FB5A/$FB60, $00FBAA..$00FCC2, $00FD40/$FD64/$FD98, $00FE3C..$00FE8E, $00FF12..$0100FE, $010122, $01014A, $010238..$0103C6, $0104B6..$0105F6, $00C348, $00C3AC | L56* (44 entries) | game/ai_l56.c | level-5/6 Cyberdyne-lab AI: equipment $0F, fixture $20/$0A, endoskeleton $27 and foot soldier $01, lab equipment $28 (HLE_RANGE_PURE, or HLE_RANGE with nested 68k sound/debris calls; bounds 1.5-36.6k from T2_HLE_BOUNDS; the code was generated instruction by instruction from the disassembly) and the hit tests $0069EE/$006AE0/$006B32/$006B68/$006B96/$006BE2/$0063C0: difftest equal on 100-200 states each (`capture_l56.txt`; $00FD64/$01000C 18/18 and 17/17, $00FB52/$00FB5A `--entry` from $00FB60 200/200); lockstep: 3000 frames identical (6 default states, `--hle all`, and all 43 states) |
| $0108E8, $010906, $0109C8, $010A92, $010AB4, $010AE0, $010AF8, $010B84, $010BE4, $010C1C, $010D46, $010DD6, $010DEE, $010E4A, $010E82, $010EF6, $010F86, $010FB0, $0062B0, $006E52 | L7* (18 states), HitSwatVan, HitL7Heli | game/ai_l7.c | level-7 freeway chase AI: SWAT van, debris, T-1000 on the police vehicle (approach, shot, fly-off, crash, passes, finale) as HLE_RANGE_PURE or HLE_RANGE with nested 68k (bounds 2.1-30k from T2_HLE_BOUNDS and dt maxima; generated instruction by instruction from the disassembly) and the hit tests (HitBox called as C): difftest equal on 3-200 states each (`capture_l7.txt`); lockstep: 3000 frames identical. Extra states `l7_chase`, `l7_van` (made by `tools/harness/scripts/l7/save.txt`) |
| $01116A, $0111CA, $0112FE, $011360, $011394, $0113D8, $011448, $0114C0, $011514, $011558, $011580, $0115AC, $011A44, $011A76, $011A90, $011AF8, $011BF4, $011C2C, $011C44, $011CD8, $006C2E, $006EF8 | L8* (20 states), HitT1000Mill, HitL8Panel | game/ai_l8.c | level-8 steel mill AI: the T-1000 (walk, attacks, action pick with its `jmp (a1)` dispatch declared as extra exits, turn, hit, down, shatter/fall, slide, flash, frozen, gone) and the liquid-nitrogen tanker (leak spawner, scroller, meter, panel, panel leak) as HLE_RANGE_PURE or HLE_RANGE with nested 68k (bounds 160-49500 from T2_HLE_BOUNDS and dt maxima; generated instruction by instruction from the disassembly) and the hit tests (HitBox called as C): difftest equal on 200/200 (Hit 72/72, Down 48/48, Fall 66/66, Gone 90/90, Flash 165/165, Shatter 2/2 + 66/66, 48/48, 165/165 `--entry`, MeterOff 1/1 + 200/200 `--entry`, Frozen also 200/200 `--entry`) (`capture_l8.txt`); lockstep: 3000 frames identical (own list and `--hle all`, default six and the l8 states). Extra states `l8_frz`, `l8_shat`, `l8_hit` (made by `tools/harness/scripts/l8/save.txt`) |
| $00E2E6, $00E314, $00E3E2, $00E546, $00E5A6, $00E72E, $00E746, $00E74A, $00E7C0, $00E864, $00E8C8, $00E91E, $00E9C4 | SkyGun, SkyGunDie, SkyIntroTiles, SkyIntroSweep, SkySpawn, SkyCtl, SkyPartIdle, SkyPartFire, SkyPartDie, SkyFinish1, SkyFinish2, SkyFinish3, SkyFadeEnd | game/ai_skynet.c | level-4 boss (Skynet CPU, type $25) AI: the four wall guns, their death tile pop, the intro (tile frames, sprite sweep), the spawner of the 16 CPU parts (HLE_RANGE_PURE, ObjAlloc as C), controller, idle/firing/dying parts, and the finish (explosions, tiles, countdown, palette fade) as HLE_RANGE with nested 68k (tile queues, sounds, CramUpload; bounds 132-30703 from T2_HLE_BOUNDS and dt maxima; generated instruction by instruction from the disassembly; `$00E79E` is never referenced and left out). difftest equal: 200/200 (Gun, IntroTiles, IntroSweep, Ctl, PartIdle, PartFire, Finish1, Finish3), GunDie 8/8, Spawn 3/3, PartDie 15/15, Finish2 2/2, FadeEnd 125/125 with `--ignore-regs d1` (d1 is the VDP status read by CramUpload, clock dependent in the sandbox); seqcmp 0 differ for GunDie, IntroTiles, IntroSweep, Spawn, Ctl, PartIdle, PartDie, Finish2, FadeEnd and both hit tests, +-2 cycle refresh-boundary noise on about 1% of the calls of the states with nested 68k (SkyGun 73/11276, PartFire 46/5676, Finish3 6/500, Finish1 1/10; no drift). Capture route `port/scripts/capture_sky.txt`; states `sky_*` made with the gun-bot (tools/harness/scripts/sky, states/INDEX.md); lockstep: identical |
| $006724, $00675E | HitSkyGun, HitSkyPart | game/ai_skynet.c | +$1C hit tests of the level-4 wall gun (one box) and CPU part (box by +$1A; call the C HitBox); 200/200 each; lockstep: identical |
| $00D112, $00D27A, $00D2AC, $00D56E, $00D4E6, $00D0D4 | TruckMain, TruckSettle, TruckSprites, TruckPanel, TruckDebris, TruckDie | game/ai_misc.c | John Connor's truck (level 3, type $1B): bouncing/settling truck (damage check, wheel and body sprites; `TruckDamage` $00D322 stays 68k via `hle_call68k`, so $D112 can run the whole destroy path), the scroller, the damage-cell panel, the flying debris pieces and the wreck. HLE_RANGE (nested 68k: TruckDamage, VdpQueueVScroll/TileRect/FillRows, sounds), bounds from T2_HLE_BOUNDS x 1.5 (TruckMain 30200 = 20088 x 1.5, TruckPanel 6500, TruckDie 15500); TruckSprites pure. `divu.w #imm` is `div_w` (GPGX UseDivuCycles + SKIP_BUS_REFRESH). difftest 200/200 (TruckMain also 18/18 on `capture_misc_t.txt`, the destroy path), 120/120, 200/200, 200/200, 200/200, 49/49 (`capture_misc.txt`; states misc_t1..t5) |
| $00DA48, $00D978, $00D9B2, $00DA18 | TrooperDA48, TrooperD978, TrooperD9B2, TrooperDA18 | game/ai_misc.c | level-1 type $00 / ground trooper $03 states (sprite trio, countdown into $D9B2, walk/shoot with the warning sound, leave); 120/120, 48/48, 200/200, 32/32; seqcmp 0 differ |
| $00B69A, $00B796, $00B7E4, $00B860 | EndoDieDebris, EndoWalk3, EndoShoot3, EndoHitDown3 | game/ai_misc.c | the endoskeleton death debris, the second walk/shoot pair and type $11 after its hit (the $C66C/$C6B0 body is inlined here: the C EndoDown charges `move.w (4,a6),(0x28,a6)` 24 instead of 20); 200/200 each (`--entry` from $B796 for $B69A and $B860); seqcmp 0 differ (+-2 on the nested-68k first instruction) |
| $00B86A, $00B896, $00B8C2, $00B92E, $00B932 | Type0DInit, Type0DSprites, Type0DWalk, Type0DDieDebris, Type0DGone | game/ai_misc.c | the rare level-1 type $0D enemy (tile rect, two sprites + walk, killed: debris and cleanup; bound 15500 = 10194 x 1.5); 200/200 each (`--entry` from $B8C2 for $B86A, $B92E, $B932) |
| $00C802, $00C826, $00C846 | CrateRise, CrateRise2, CrateRise3 | game/ai_misc.c | the crate rise states left out of ai_simple.c ($C8A8 inlined); 26/26, 8/8, 18/18; seqcmp 0 differ |
| $00627A, $005ABC, $005E1C | HitTruck, HitType11, HitType0D | game/ai_misc.c | +$1C hit tests of the truck (two boxes), type $11 and type $0D (call the C HitBox); 136/136, 48/48, 27/27; seqcmp 0 differ. States misc_* (tools/harness/scripts/misc, states/INDEX.md); lockstep: identical |
| $005988, $00598E, $0059E6, $005A02, $005A54, $005A5A, $005B10, $005B2E, $005C0A, $005C10, $005E28, $005F1A, $005F34, $005F7A, $005F80, $005FDE, $006022, $006028, $006146, $006154, $006194, $0061A2, $0061D6, $0061E4, $006214, $00621E, $00628E, $00629E | Onhit1_* (28) | game/onhit_l1.c | the level-1 on-hit and kill callbacks that ShotHitTest reaches by `jsr (a0)` (prop, endoskeletons, human, wall, truck, HK tank parts and weak point); nested HLE dispatch inside `hle_call68k` works (as for HitBox). Lockstep: 6 default states and all 92 states `--hle all` identical, the 28 alone identical; every one is hit in those states. Left out (never reached by any state): $005A00 $005A08 $005AC8/E $005B56 $005BBE $005E36 $006166 $0061BC. seqcmp: the 15-register `movem.l` push/pop of $006252/$005B2E reads 2 cycles low in the original log (SKIP_BUS_REFRESH artifact of the log, the lockstep hashes are exact) |
| $0062FA, $0063CC, $0063D2, $006A00, $006A06, $006AEC, $006AF2, $006B3E, $006B44, $006BA2, $006BEE, $006C42, $006F04, $006F5A | Onhit5678_* (14 of the 24 written; the rest are under `#ifdef ONHIT5678_UNPROVEN`) | game/onhit_l5678.c | the level-5/6/7/8 on-hit and kill callbacks the hit tests of ai_l56.c/ai_l7.c/ai_l8.c hand to ShotHitTest (equipment, fixture, endoskeletons, model, SWAT van box 1, steel-mill T-1000, tanker panel). Dispatch counts over 92 states x 3000 frames (`T2_HLE_TRACE=1`, `--hle all`): Equip 278, EquipKill 132, Endo2 97, Endo2Kill 21, Endo 36, EndoKill 3, Lamp 3, LampKill 2, Model 8, Model2 4, Van1 64, Mill 3, Panel 389, PanelKill 389. Never reached by any state, so left out (#ifdef): $0062D4 $0062DC $00630A $006B74 $006B7A $006BD8 $006C24 $006C5E $006E66 $006E7E. difftest (`--n 50`, capture_l56/l7/l8): Endo 50/50, EndoKill 18/18, Equip 50/50, EquipKill 50/50, Model 14/14, Model2 5/5, Endo2 50/50, Endo2Kill 17/17, Van1 50/50, Mill 6/6, Panel 50/50, PanelKill 50/50 (Lamp/LampKill not on the capture route: seqcmp 1/1 each on pad_05, 0 differ). Lockstep 3000 frames identical: own list (six default + pad_05 2p_pad_05 l8_hit l8_shat l7_van l7_chase), `--hle all`, and all 92 states `--hle all`. seqcmp 0 differ except EquipKill 1 call 2 cycles low on the nested sub_0072CE movem (log artifact, hashes exact) |
| $009486..$00AE72 (49 handlers: ops $01 $02 $07-$0E $12 $13 $15 $19 $1B $1D-$20 $24 $26-$2F $30-$3C, queue_gfx $0F-$11, draw_tiles $1A/$1C, load_tiles $25, clear_tiles $28/$29, copy_tiles $2A) | ScrSpawnT*, ScrQueueGfx*, ScrLoadTiles, ScrDraw*, ScrClear*, ScrCopyTiles* | game/scriptspawn.c | level-script spawn/tile handlers, generated instruction by instruction from the disassembly (cost = GPGX table entry of each opcode word; `costcheck.py`: 1289 charges direct, 0 bad). All HLE_FUNC_PURE: the callees (ObjAlloc/Random/ObjSpawnTemplate as C, sub_00B488.., VdpQueueTileRect, sub_013D04, TruckDamageInit.. as static C) only touch work RAM/ROM. difftest equal on every handler the route `capture_spawn.txt` reaches (e.g. 139/139 ScrLoadTiles, 42/42 T01d, 30/30 T09, 22/22 T0fa, 5/5 T13a); the rest (ops unused in the first ~3000 frames) by lockstep; seqcmp: the call sequences equal up to a one-call offset (the original run has an extra first call). Lockstep: 3000 frames identical; dispatch counts over the 92 states x 3000 frames (`--hle all`, `T2_HLE_TRACE=1`): every handler 2..511 calls except ScrSpawnT17 ($1B, the level-1 HK tank, only spawned late in level 1), covered instead by difftest 1/1 + 7 object-table-full variants (`--poke` filling the first 0..6 slots), cycles equal |
| $00651E $006550 $006582 $0065B0 $006668 $0066B4 $006846 $0068A0 $0068D6 (+ kills $006524 $006556 $006588 $0065B6 $006686 $00684C $0068A6 $0068DC), $005CD2 $005D9C $005DCC $006060 (+ $005CD8 $005DA2 $005DD2 $006066), $00639A $006430 $006462 $0064A0 $0067D2 $006806 $00690C (+ $0063A0 $006436 $006468 $0064A6 $00680C), $006770 $00677E | Onhit2_*, Onhit3_*, Onhit4_*, OnhitSky_* (39) | game/onhit_l234.c | the level-2/3/4 and Skynet on-hit and kill callbacks the hit tests load into a2 (and the kill routines they hand back); same scheme as onhit_l1.c. Lockstep: 6 default states, `--hle all` and all 92 states `--hle all` (4 parallel copies) identical; every one is hit in those states. dt: $006430 46/46, $00690C 6/6 (capture_l4b; the rest are too rare on the capture routes); seqcmp 0 differ for $00690C $006430 $006462 $006806 $0068A0, while $006436 and $006770 differ by 2 cycles on a movem (the same log artifact as noted for onhit_l1; hashes exact). Left out (never reached): $0065DE/$0065E4 $006612/$006630 $0066BA $006332/$006346 $0067D8 $0069A2 $006730/$00673E |
| $00074C, $00076A, $00086C, $00700C, $008FA8, $00909C, $0090B2, $009104, $009186, $007DEE | PlayersButtons, PlayerButtons, PlayerShoot, PickupShotTest, SetPlayerState, PlayerDamageNearest, PlayerDamage, EnergyHud, GunpowerHud, PrintText | game/player.c | player input, shooting, damage and HUD, generated instruction by instruction from the disassembly (`costcheck.py`: 343 charges direct, 0 bad). ShotHitTest/PlayerButtons/PlayerShoot/PickupShotTest/SetPlayerState/PlayerDamage are called as C; sounds, impact sprites, the sub_007B* state sprites, the bar writer sub_007EB2, VramWriteWord (the text) and the PickupHitTable routines run as 68k via `hle_call68k`, so every VDP write keeps the 68k timing (not pure; bounds = observed max x 1.5 from `T2_HLE_BOUNDS=1 T2_HLE_NODECLINE=1` over 92 states x 3000 frames: 46800, 12100, 29800, 3300, 10200, 14800, 14700, 5400, 5500, 222500 (PrintText: one call can print for >1 frame, so it mostly runs as 68k when interrupts are enabled)). Dispatch counts over 92 states x 3000 frames (`T2_HLE_TRACE=1`, `--hle all`; PlayerButtons/PlayerShoot/PickupShotTest/PlayerDamage are normally reached by direct C calls from PlayersButtons/PlayerShoot/PlayerDamageNearest, so their dispatch counts come only from the declined (68k) parents; with `-PlayersButtons`: 510112 each): PlayersButtons 255059, PlayerDamageNearest 82570, EnergyHud 255095, GunpowerHud 255095, PrintText 2240, SetPlayerState 32, PlayerDamage 11157 (declined parent), PickupShotTest 1919 (declined parent). difftest (`--n 50`, capture.txt Menacer route): 50/50 each (PlayersButtons, PlayerButtons, PlayerShoot, PickupShotTest, PlayerDamageNearest, PlayerDamage, EnergyHud, GunpowerHud, PrintText), SetPlayerState 3/3 x `--poke D0=1..6`, PlayerDamage/PlayerDamageNearest 50/50 x `--poke D2=0xFFFF/0x1FFF/0x8000/0x4000` (death, ENERGY LOW, hit paths). Lockstep 3000 frames identical: own list and `--hle all` (six default states) and `--hle all` over all 92 states (4 groups); mousetest ALL PASS; seqcmp (2p_gun_04): PlayerButtons, PickupShotTest, PlayerDamageNearest, PlayerDamage, EnergyHud, GunpowerHud 0 differ, SetPlayerState (gun_07) and PrintText (gun_05, gun_0a) 0 differ; PlayersButtons 29/1453 and PlayerShoot 40/2906 calls differ by 2 cycles on one instruction of the nested sound routine $013D04 (the same movem log artifact as onhit_l1, hashes exact) |
| $0087C4, $007D0E, $007C9A, $007CD8, $007EB2, $007E08 | VramWriteWord, DigitPrint, HudScores, HudLives, StringPrint, TextPrint | game/textprint.c | the text/HUD printing cluster: VramWriteWord (VRAM[d2] = d0.w), the BCD digit printer sub_007D0E (it has no leading-zero blanking in the ROM: every nibble is tile $1A + digit + base $FF9E2C), the HUD score/lives printers sub_007C9A/sub_007CD8 (call sub_007D0E, the second as a tail jmp/bra), the string printer sub_007EB2 and the text printer sub_007E08 (newline $80+ and CR codes, letters/digits/space/punctuation table). VDP-writing, so they copy the exact timing of `game/vdplist.c` ("Exact timing" above): every instruction advances the real clock (`pre`/`post`/`tick`, refresh check before each), the port writes are timed (`wr_ctrl`/`wr_data`, FIFO stalls), each function runs in an exact session (`sess_begin`, HLE cycles 0) and is guarded by `safe_run(bound)`: it declines (the 68k runs it) when VBlank-set/clear, the frame wrap, H-Int, the light gun line or a DMA could fall inside. Bounds are per-character upper bounds (320 cycles per tile written, 400 for VramWriteWord; the string printers scan the 0-terminated string first, > 4096 / 2048 bytes declines), so a loose bound only means more 68k. VramWriteWord is a static body `vww` called directly by the printers (their `jsr` charged, the `rts` inside it) and is also the registered function, so 68k callers and the C callers that use `hle_call68k(0x0087C4)` (vdplist.c command 3, player.c PrintText) dispatch to it nested in their session; the printers are called from the 68k and by C only through the nested dispatch (PrintText still falls into sub_007E08 inside player.c with its own timeline). All registers (widths, a5 = $C00000, d1/d2 scratch), the final CCR and the charge are mirrored; `costcheck.py` on a copy with `tick`/`wr_*` mapped to `hle_insn`: 105 charges, 97 direct, 0 bad. difftest 200/200 each (6 functions); lockstep 3000 frames identical: own list, own list + `--z80`, `--hle all`, `--hle all --z80` (six default states) and `--hle all` over all 92 states (wide gate); pause_gate 5738, levelflow_gate 43913, screens_gate 34882 frames identical; mousetest ALL PASS |
| $0080FE, $008782, $00873A, $00874E, $0086C4 | CramUpload, VramFillWords, VramFillBody (sub_00873A), VramFillTail (sub_00874E), RamClear | game/vdpfill.c | the VDP upload helpers. The VDP ones use the exact timing of `textprint.c`/`vdplist.c` (`pre`/`post`/`tick`, timed `wr_data`, exact session, `safe_run`). Their `move.w (4,a5),dN / btst / bne.s` status polls read the VDP at their own clock with the 68k's prefetch bits ORed in (`vdp_status_word`), and set REG_IR to the reading opcode first (`gx_m68k_set_ir`): the core's status read adds `m68k_cycles()` = the cost of the opcode in REG_IR, a stale value from C changes the FIFO-full/VINT/HBLANK bits. CramUpload (64 words, ~3400 cycles) is guarded by `safe_run` and declines (the 68k runs it) when a frame-loop event could fall inside. The fill loops are long (VramFillBody+Tail ~280000 cycles, VramFillWords up to a plane), so at top level they use the **bail** of vdplist.c: `due()` is checked before every instruction and at the first boundary at or after the next event every register and the CCR are mirrored and the call jumps into the original code there (`hle_ctx.bail`); the head (450 cycles) is guarded by `head_ok`, and the 873A body bails to $00874E when its head would not fit. Called nested (VdpCmd2FillZero/VdpCmd5FillValue `hle_call68k`, which already guard the call) the whole fill must fit (`safe_run`) or the 68k runs it. RamClear is a plain HLE_FUNC_PURE timeline (32710 x move.w (a0)+/cmpa.l/bne.s, ~1.06 Mcyc, `memset`); it only runs at boot, before the snapshot every gate state is taken from, so it has no dt state in the routes: `port/scripts/capture_boot.txt` (`run 10`) captures its one call (`difftest capture-multi 5 v 0086C4 --script port/scripts/capture_boot.txt --out runs/rc`), and a cold boot of 900 frames with `--hle RamClear` has the same `--framehash` as `--hle none`. **Sandbox note:** the difftest reference run only moves the VDP line base, not the V counter (`gx_vdp_follow_cpu`), so these functions use that in the sandbox (`follow()`) and `gx_vdp_follow_cpu_lines` otherwise, else a status read's VINT bit differs for a call that spans a line; the lockstep and route gates are the proof for the V counter. Not ported: VramClearDma $00800C (a 65520-byte DMA fill and a ~660k-cycle busy poll across five VBlanks) and DmaToVram $011FA2 (DMA that halts the 68k per line): their time is the frame loop's DMA stepping, which a C function cannot interleave exactly (docs/remaining.md). difftest `capture-multi 60 v 0080FE 008782 00873A 00874E --script port/scripts/levelflow_gate.txt --script port/scripts/screens_gate.txt --out runs/vf`: 60/60 each (CramUpload, VramFillWords, VramFillBody, VramFillTail), RamClear 1/1; `costcheck.py` on a copy with the helpers mapped to `hle_insn`: 49 charges, 15 direct, 34 aligned, 0 real mismatches (2 checker artifacts: the dbra expiry 14 aligned to CramUploadChanged, an unanchored helper); lockstep 3000 frames identical: own list, own list + `--z80`, `--hle all`, `--hle all --z80` (six default states) and `--hle all` over all 92 states (wide gate, 92 identical); pause_gate 5738, levelflow_gate 43913, screens_gate 34882 frames identical (own list too: levelflow/screens); mousetest ALL PASS; make OK; romscan CLEAN. Cold-boot `--framehash` with `--hle all` differs from `--hle none` from frame 241 with or without these functions (not caused by them) |
| $0123F0, $01243C, $0126B4, $012886 | MissilesRun, EffectsRun, SparksRun, DebrisRun | game/pools.c | the object-pool runners (docs/subsystems/objects.md), generated instruction by instruction from the disassembly. Effects/Sparks/Debris are HLE_FUNC_PURE; MissilesRun is an HLE_FUNC (states 1..7 inlined through the `bra` table; the hit states call ShotHitTest via hle_call68k), bound 39000 = T2_HLE_BOUNDS observed max 25883 x 1.5 (92 states x 3000 frames); a frame with a state word > 7 is declined. difftest 50/50 each; seqcmp 0 differ (Effects/Sparks/Debris gun_05+gun_01, Missiles gun_01); Missiles gun_05 shows 41 differing calls in the seqcmp nested-original environment only (long calls that straddle slice ends; the real-flow lockstep is identical). Lockstep: `--hle MissilesRun` 3000 frames identical on gun_05/gun_01; `--hle all` identical on all 92 states (4 copies on the VM); mousetest ALL PASS |
| $01237A, $012352, $01248C..$012568 (spark spawners, 10), $01272A..$01277C (debris spawners, 8), $01290E, $012926, $01293E, $012956, $01296E, $01257A, $012502 | SpawnBomb, BombStart, SparkSpawn*, SparkRand*, DebrisSpawn*, DebrisTriple, *Alloc | game/pools.c | leaf spawners and pool allocators the AI states call (HLE_FUNC_PURE; those ending in `jmp sub_013CC4` are HLE_RANGE_PURE to that jump, the sound stays 68k). difftest equal: DebrisSpawn 50/50, DebrisSpawn280E 5/5, SparkSpawn/269A 33/33, SpawnBomb 16/16, BombStart 50/50, Missile/Pickup/Spark/Effect/DebrisAlloc 50/50, 11/11, 33/33, 6/6, 50/50; the ones the routes never reach (2806, 27FE, 27F6, 2816, 281E, Triple, SparkRand*, SparkSpawn*Snd*, 618) by `--entry` from $01277C / $01257A captures, 50/50 or 33/33. Dispatch counts (92 states x 3000 frames, `--hle all`, T2_HLE_TRACE=1): runners 254480 each; MissileAlloc 14845, SparkSpawn269A 2998, SparkSpawn618 2131, DebrisSpawn 919, SparkRand65CSnd67 542, BombStart 329, EffectAlloc 325, SparkSpawn5B6Snd6A 233, SparkRand618 237, SparkSpawn618Snd68 205, SpawnBomb 113, PickupAlloc 408, SparkRand5B6Snd67 62, DebrisSpawn280E 40, DebrisSpawn27F6 36, SparkRand5B6 33, SparkSpawn 28, SparkAlloc 19, SparkRandVel 17, DebrisSpawn27FE 6, DebrisTriple 5, DebrisAlloc 5, DebrisSpawn2806 3; never dispatched there: DebrisSpawn281E ($01274E), DebrisSpawn2816 ($012754), SparkSpawn65CSnd67 ($012568) (kept, `--entry` only) |
| $00A140 + 45 targets (see below) | ScrNative, ScrLoadPalette $00A2A8, ScrDemoSkip $00B35A, ScrSetPlayers $00B2FA, ScrQueueFillRowsE214 $00B342, Nat00xxxx | game/scrnative.c | level-script `native` op ($14) and its targets, generated instruction by instruction from the disassembly (`costcheck.py`: 673 charges, 0 bad). ScrNative is `HLE_RANGE_PURE` that stops in front of its `jmp (a1)` (the real 68k jumps and the target's hook takes over); $00B342 is a range in front of its tail `jmp VdpQueueFillRows`. 41 targets are `HLE_FUNC_PURE` (callees as static C copies); the three that call the 68k use `hle_call68k`: Nat009290 (PrintText, bound 12000 = NODECLINE max 7965 x 1.5), ScrLoadPalette (CramUpload, 5400 = 3598 x 1.5), Nat00A4B2 (sub_0081B6, 1750 = 1161 x 1.5). Dispatch counts over 92 states x 3000 frames (`T2_HLE_TRACE=1`, `--hle all`): ScrNative 340, Nat00D0F8 78, ScrSetPlayers 44, ScrDemoSkip 27, Nat00A4F4 25, Nat00A712 16, ScrLoadPalette 13, Nat00AEFA 13, Nat00AF1C 13, Nat00A510 9, Nat00A25A 9, Nat00A2D4 9, Nat00A304 9, Nat00A33A 9, Nat00A4B2 9, Nat009290 8, Nat00A24E 8, Nat00B200 7, Nat00AFD4 6, Nat00ACB0 5, Nat00AD48 5, Nat00AD9C 5, Nat00AFFC 5, Nat00B02A 5, Nat00B0B2 5, Nat00B1CE 5, Nat00A41A 4, Nat00A46E 4, Nat00A4D2 4, Nat009F0C 4, Nat00A82C 4, Nat00A85E 4, Nat00A880 4, Nat00A914 4, Nat00AB4E 4, Nat00AB62 4, ScrQueueFillRowsE214 4, Nat013E04 3, Nat00AB76 2; never reached by any state (exercised by `--entry` only): Nat00A266, Nat00A270, Nat00A31A, Nat00A32A, Nat00A3F4, Nat00A8BC, Nat00A94A. difftest: ScrNative 50/50 (`capture_spawn.txt`, `--n 50`); every target `--entry` from those states with `--poke "A0=<script args>,A1=<target>"` for each script occurrence (levelscript.py addresses +8): 50-450 states equal each (ScrLoadPalette 300, ScrDemoSkip 300, ScrSetPlayers 450; A2D4/A304/A31A/A32A also `L:FF9ECA=` and AFD4 `L:FF9EDA=` a slot, since the sandbox state has no boss pointer). seqcmp 0 differ on all 39 tried except Nat009290 (call 0: one nested PrintText `movem.l` logged 22 vs 24, the known log artifact, hashes exact). Lockstep 3000 frames identical: own list and `--hle all` (six default states) and `--hle all` over all 92 states (4 groups); mousetest ALL PASS. Not ported: $013C74 (runs 248 frames of ScrollRows/ObjRunAll/VdpListRun inside one call, so every VBlank interrupts it and a C version could never run) |
| $000308, $00039E, $013D04, $013CC4, $013D56, $0FBAD2, $0FBAE2, $0FBB6C, $0FBB96, $0FBBAE, $0FC0FE, $0FC27C | VBlankInt, ExtInt, SndRequest, SndMusicRequest, SndSlotRequest, Snd_* | game/vblank.c | the VBlank handler (Menacer port-2 handshake with per-iteration dbra charges, latch copy, vblank_count++) as a range from $000308 to right before `jsr sub_0FBBD0` ($000396; the sound engine tick and the rte stay 68k, as does the Z80-bus/YM code in it), the Menacer TH handler ExtInt (HV counter read through hle_pre/post, range to its `rte`), and the work-RAM-only sound request routines (HLE_FUNC_PURE; sub_0FBB96/0FBBAE/0FC0FE/0FC27C are called as static C). `costcheck.py`: 135 charges, 0 bad. Bounds: VBlankInt 14000 (constant 13292 cycles, entered with mask 6 so only the frame-end check uses it; the T2_HLE_BOUNDS span for a range stops at the first flush and reads 90), ExtInt 220 (observed max 144 x 1.5). difftest: VBlankInt 200/200, SndRequest 200/200, Snd_0FBB96 200/200, SndMusicRequest 12/12, SndSlotRequest 3/3, Snd_0FBBAE 12/12, Snd_0FC27C 3/3, Snd_0FC0FE 18/18, Snd_0FBB6C 1/1, Snd_0FBAD2 7/7 and Snd_0FBAE2 6/6 (pause route); ExtInt differs only in the H-counter byte it stores in the ring (the sandbox does not advance the clock between the 68k instructions, so the HV counter cannot match there; lockstep is the proof). lockstep gains `--z80` (Z80 RAM in the compared hash, `T2_HASH_Z80`): 3000 frames identical with it, own list, `--hle all` (six states) and `--hle all` over all 92 states (4 groups); pause_gate 5738 frames identical; mousetest ALL PASS; seqcmp 0 differ (ExtInt, SndRequest, SndSlotRequest, Snd_0FBB96/6C/C27C), VBlankInt gun_05 325/999 calls differ by 2 cycles on the closing `movem.l (sp)+` (the known movem log artifact, hashes exact). Dispatch (600 frames, `T2_HLE_TRACE=1`): VBlankInt 599-600, SndRequest 33-49 |
| $00C48E, $012234, $01227A, $012BB0, $008C8E, $008C52 | ObjPathStep, VdpQueueTileRect, VdpQueueTileRectMirror, PickupsRun, PlayerState, PlayersState | game/misc2.c | the work-RAM-only leftovers of docs/remaining.md "Other", generated instruction by instruction from the disassembly. All HLE_FUNC_PURE: ObjPathStep (object path stepper; registered at its own address so the 68k callers get C, the AI files inline their copy; bound 600), VdpQueueTileRect/Mirror (VdpList command 6 builders, 4000), PickupsRun (pool $FF9AB2, 4000). PlayerState/PlayersState port the idle path only (bounds 200/400): any state word 7, state 1 with timer 120 or 50, state 5 with timer multiple of 16, or timer 1 (the blocking waits, sounds, ReadInput) declines to 68k. difftest (`--n 200`, capture.txt): ObjPathStep 200/200, VdpQueueTileRect 43/43, PickupsRun 200/200, PlayerState 200/200, PlayersState 200/200; VdpQueueTileRectMirror is never called on the capture route (no states; covered by lockstep only, if reached). Lockstep 3000 frames identical: six default states (own list), `--hle all` and `--hle all --z80`, and `--hle all` over all 92 states (wide.sh); pause_gate 5738, levelflow_gate 43913 and screens_gate 34882 frames identical; mousetest ALL PASS. (Run gates only with states/ copied in: a vmrun sync deletes the untracked states, and levelflow_gate then diverges at frame 45931 because its `load states/...` lines silently find nothing.) |
| $0FBBD0, $0FBBFE, $0FBDDC, $0FBDE2, $0FBDF0, $0FBE18, $0FBF08, $0FBF26, $0FBF58, $0FC0A6, $0FC0F0, $0FC10C, $0FC184, $0FC194, $0FC2AA, $0FC2D6, $0FC2DA, $0FC408, $0FC428, $0FC47A, $0FC488, $0FC496, $0FC4FA, $0FC59C | SndTick, SndTickBody, SndChanUpdate2, SndChanDone, SndChanUpdate3, SndChanUpdate, SndNoteFetch0, SndNoteFetch, SndEvent, SndClearChan, SndClear0F0, SndSfxTick, SndSfxStop, SndSfxFetch, SndSlotFlush, SndZ80Cmd1E, SndZ80Cmd, SndBusReq, SndBusRel, SndZ80Cmd2, SndZ80Cmd4, SndUploadCheck, SndFlagCheck, SndWaitZ80 | game/sound.c (generated by `tools/py/sndgen.py`) | the 68k side of the sound driver: the per-frame tick (wrapper that takes the Z80 bus, tick body, channel update / note fetch / event reader, sfx tick, command slots, the Z80 command helpers, BUSREQ acquire / release and the wait for the Z80's command byte). Generated instruction by instruction from the disassembly (objdump with relocations for the `dat_0F8xxx` ROM operands, GPGX opcode cycle table; `costcheck.py`: 879 charges, 0 bad), all routines in one function `snd_run()` entered through a switch on the entry address; `bsr` is a recursive call (the return address is pushed and popped, so the stack bytes match), `rts` returns, branches are gotos, a fall-through or tail `bra` between routines is just code order. Every instruction is on the timeline: an instruction whose effective address is not work RAM or ROM (Z80 RAM through a6, `$A11100` BUSREQ, `$A10001`) is bracketed by `hle_pre()` / `hle_post(n)` with the original's cost, decided at run time from the address (a6 is read from `$FF0000`), everything else is `hle_insn(n)`; `movem.l` uses `hle_movem_l`. The BUSACK spin (`$0FC408`), the `$A11100` poll in the wrapper and the command-byte wait (`$0FC59C`, which releases the bus, burns 11 nops and requests it again each pass) are the original loops, one pass per iteration with the original charges, so the Z80 runs exactly where it does in the original: nothing was left to the 68k. Not pure (Z80 bus), so the layer judges the entries by their declared bound (`SND_BOUND` 6000 = difftest max 5356 for the whole tick); the tick is entered from VBlankInt with mask 6, so only the frame-end check uses it, and the entry sits at ~96000 mclk of a 896040 mclk frame, so it never declines (T2_HLE_DECLINES: 0 in 4 states x 1500 frames). `T2_HLE_BOUNDS` reads low (SndTick max obs 496): the timeline flushes at every Z80 access, and the checker stops at the first flush. difftest: SndTick 200/200, SndTickBody 200/200, SndChanUpdate2 200/200, SndChanDone 200/200, SndChanUpdate3 200/200, SndChanUpdate 200/200, SndNoteFetch0 200/200, SndNoteFetch 200/200, SndEvent 200/200, SndClearChan 82/82, SndClear0F0 200/200, SndSlotFlush 5/5, SndZ80Cmd 5/5, SndBusReq 200/200, SndBusRel 200/200, SndZ80Cmd2 2/2, SndUploadCheck 200/200, SndFlagCheck 200/200, SndWaitZ80 200/200 (capture.txt); SndSfxTick 40/40, SndSfxStop 16/16, SndSfxFetch 40/40, SndZ80Cmd1E 40/40, and SndSlotFlush/SndZ80Cmd/SndZ80Cmd2/SndClearChan again 40/40 (`capture-multi 40 snd` over levelflow_gate + screens_gate + pause_gate; states in `runs/states/<ADDR>_snd`, `difftest run FUNC --states runs/states/<ADDR>_snd`). SndZ80Cmd4 ($0FC488) cannot be difftested: it ends in the wait for the Z80's command byte, and the sandbox never runs the Z80 (the original does not return there either); it is covered by lockstep only. Lockstep 3000 frames identical, all WITH `--z80` (Z80 RAM in the hash): own list (all 24), the 22 entries without SndTick and SndTickBody (so each is dispatched from the 68k tick), SndTick alone, `--hle all` (six default states), and `--hle all` without `--z80`; `/opt/t2/bin/wide.sh` (`--hle all`, 92 states x 3000 frames) 92 identical, and the same wide run with `--z80` added (`lockstep.sh --z80`, 4 copies) 92 identical; pause_gate 5738, levelflow_gate 43913 and screens_gate 34882 frames identical; mousetest ALL PASS; make OK. Dispatch (4 states x 1500 frames, `T2_HLE_TRACE=1`, `--hle all`): SndTick 6000 (once per frame; the others are C-to-C calls); with SndTick/SndTickBody off, so that each entry is dispatched from the 68k tick: SndChanUpdate3 3734, SndUploadCheck 3333, SndFlagCheck 1630, SndNoteFetch0 1229, SndChanUpdate2 1229, SndSfxTick 813, SndBusReq 478, SndBusRel 478, SndClear0F0 404, SndClearChan 140, SndZ80Cmd2 37, SndZ80Cmd4 37, SndSlotFlush 7. Not ported (boot only, not reached by the tick): the driver reset / Z80 loader $0FBB00, $0FC32E, $0FC350, $0FC3E6, $0FC438 and the Z80 program blob at $0FC5CC. $0FC0FE / $0FC27C / $0FC290 stay registered as the vblank.c pure routines (the same code is also inside `snd_run` for the callers in the tick) |
| $013272, $013296, $0132C4, $0132E8, $013340; $00498A; $004752, $004770, $004792, $0047AE; $0123F0, $012402; $00074C, $00076A, $00086C, $009104, $009186; $0120BA, $0120DA, $0120F6, $012068, $0120B0, $011E48, $011E7E, $011E8A, $011EB4 | BuildSpriteTable (+Sort/Sprite/Piece/Tail), BgStreamLayers, ScrollRows (+Group/Row/List), MissilesRun (+MissilesSlot), PlayersButtons, PlayerButtons, PlayerShoot, EnergyHud, GunpowerHud, VdpCmd6TileRect (+Row/Words), VdpCmd1CopyWords (+CopyLoop), VdpCmd7HScroll (+A/Setup/B) | game/sprite.c, scroll.c, pools.c, player.c, vdplist.c; port/hle | early bail and resume (section above): the functions that were declined for an interrupt inside the call (25.2% of busy ran as 68k) run in C up to the last bail point before the event and, for the loops, take over again at the next loop head. difftest (`port/tools/dt`, 200/200 each, VdpCmd6TileRect 43/43 = all the route has): all 28 entries equal; the bail paths are only reachable in the frame loop, so lockstep is their proof (see the gate lines in the commit). |
