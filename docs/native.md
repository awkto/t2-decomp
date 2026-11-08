# The native runner: no 68000 emulation (GitLab t2-decomp#11, phase P5)

Goal: run T2 with no 68k instruction emulation, as the sibling project Scooby does (`scooby-decomp/docs/native.md`).
The game logic is the C of `port/game/` (the HLE functions, ranges and glue blocks), run by address by a small
dispatcher, the *native runner* (`port/hle/native.c`). The rest of the machine stays GPGX's (VDP, Z80 with the
YM2612/PSG, the pads, the interrupt lines), and work RAM is still GPGX's 64 KiB array.

Status (wave 2, "Cold boot and the census-driven blocks" below): 50 of the 92 wide-gate states run their 3000 frames
with no 68k instruction at all; every other run, and all five route gates, stop at the two DMA routines
(`VramClearDma` $00800C, `DmaToVram` $011FA2), which are another task's. Everything else the 97 runs reach is C.
Without the DMA routines the runner stops with `no C block at $ADDR`; `T2_NATIVE_FALLBACK=1` lists what is missing.

## How it runs

```
GPGX frame loop (system_frame_gen, per raster line)           port/hle/native.c
  m68k_run(line_end) ───────────────────────────────────►    native_run(cycles):
                                                                take a pending interrupt (vector -> C block)
                                                                while the slice has time:
                                                                  C block at PC (hle.c dispatch -> run_def)
                                                                  PC = its exit / the popped return address
                                                                  clock += the cycles the block charges
                                                                no C block at PC -> fatal ("no C block")
```

- `T2_NATIVE=1 build/port/t2port ...` is the emulated build with the 68k core linked but **never stepped**:
  `tools/patches/gpgx-native.patch` routes the core's `m68k_run`, `m68k_step_one`, `m68k_set_irq_delay` and the
  SR accessors to the runner while `native_active` is set, and counts the instructions the core executes
  (`native: N C blocks run, M instructions executed by a 68k core` at exit; M must be 0). Without the variable
  nothing changes: every gate runs as before.
- Every `HLE_FUNC` / `HLE_RANGE` is a C block under its 68k address (`port/hle/hle.h`). A block leaves through
  `hle_jump` (a range's declared exit, a computed target) or the implicit `rts`. The runner looks the next PC up
  and runs that block; an address with no block is fatal, with the address, the frame and the stack.
  `hle_call68k_orig` and `hle_run68k_until` are fatal too (they would run the original).
- **No declines.** `hle_native_mode()` turns the interrupt-atomicity machinery off (docs/porting.md "Interrupts
  inside a replaced function", "Early bail and resume"): there is no 68k to hand a call back to. Interrupts
  are taken between blocks, so a function that is long enough to straddle one runs to its end first. Closing
  that difference is the interrupt-inside-C work (a separate task).
- **Time.** A block charges the original's cycles; the runner adds them to the CPU clock GPGX schedules the VDP,
  the Z80 and the YM2612 against. Nested calls from C (`hle_call68k`) run C blocks as well.
- **Interrupts.** VBlank (level 6), HBlank/ExtInt (levels 4/2) are taken at block boundaries: the runner pushes the
  exception frame and jumps to the vector, `VBlankInt` / `ExtInt` (registered C ranges) then `rte`
  (`glue.c`). A new SR that unmasks a pending interrupt takes it at once, as the 68k core does.
- **Waits** (`port/game/natwaits.c`, group `native`). The game waits for the VBlank in spin loops on
  `vblank_count` ($FFA0EE). With a 68k these loops stay the original code (their instruction phase decides at
  which boundary the interrupt is taken, which lockstep depends on), so the blocks exist only in the
  native runner (`HLE_WAIT_FUNC_GROUP`, enabled by `hle_native_mode()`): a block tests the condition like the
  original; while it holds it calls `hle_idle()` (charges the rest of the CPU's time slice) and jumps to itself.
  The frame loop moves on, delivers the interrupt and the block runs again.

## Glue (`port/game/glue.c`, generated)

The pieces of earlier waves (`piecegen.py`) stop in front of every `jsr`/`bsr`/`jmp`/`rts` and leave the
instruction to the 68k. `tools/py/gluegen.py` turns each such instruction into a block of its own: the cycles of
the 68000 instruction (`hle_insn`, exact), its effect on the stack and PC, and `hle_jump` to the target. They are
the same on the emulated build (the block replaces the instruction with the same charge: lockstep identical)
and in the native runner. Static targets are `HLE_RANGE_PURE`s with the callee as an exit at SP-4, computed
targets (`jsr (a0)`, `jmp (a0)`) are functions that jump. The site list is `data/glue_sites.txt`; it grows from
the census (`runs/ipprof/glue_candidates.txt`, written by `remaining68k.py`).

```
port/tools/ipprof.sh                                  # on a VM: census over every gate route (T2_IPPROF)
python3 -I tools/py/remaining68k.py > docs/remaining68k.md
python3 -I tools/py/gluegen.py --add > port/game/glue.c      # add the barrier instructions the census found
OUT=runs/native T2_NATIVE=1 port/tools/ipprof.sh      # the same routes on the native runner
grep -h "no C block" runs/native/work/*.log | sort | uniq -c | sort -rn
python3 -I tools/py/nativecensus.py --strict runs/native/work   # per route: frames before the first stop
```

## The census (docs/remaining68k.md)

`port/src/ipprof.c` (`T2_IPPROF=file`, `%d` = pid) counts every 68k instruction the emulated CPU interprets per
address, top level and nested in a C function, and per replaced entry how often it was reached, declined and
bailed. `tools/py/remaining68k.py` groups the addresses by reason:

| reason | meaning |
|---|---|
| declined/bailed tail | the function is C, but the call was handed back to the 68k (interrupt inside, `HLE_F_BAIL`); closed by the interrupt-inside-C work |
| no C block: function not ported | no `HLE_*` entry inside the function |
| barrier jsr/bsr/jmp/rts/rte between C pieces | closed by `glue.c` |
| gap in a ported function | an instruction no piece covers |
| wait spin: C block in the native runner only | `natwaits.c` |

## Results (end of the infrastructure step, wave 1)

Census over 97 runs (the 92 wide-gate states x 3000 frames and the five route gates, 423,301 frames), `--hle all`:

| | before | after |
|---|---|---|
| distinct 68k addresses interpreted | 9827 | 5953 |
| interpreted instructions, top level / nested | 2,596,124,938 / 14,431,437 | 2,573,501,147 / 12,811,990 |
| of which wait spins (native-only blocks, idle) | 2.50 G | 2.50 G |
| barrier jsr/bsr/jmp/rts/rte addresses | 1282 | 34 |
| functions with no C block at all | 377 | 6 (+ the 96 hardware / sound-boot / spin functions `piecegen auto` leaves) |

The remaining rows are mostly "declined/bailed tail" (the interrupt-inside-C task) and the hardware functions
(VDP/DMA/Z80/I-O ports: `VramClearDma`, `VsramClear`, `DmaToVram`, `VdpQueueVScroll`, the sound driver's Z80 boot
code, `Reset`), which need hand-written C with exact port timing.

Native runner (`T2_NATIVE=1 T2_NATIVE_SETTLE=1`, 97 routes, `OUT=runs/native port/tools/ipprof.sh`): before this step
every route stopped at the first wait spin (frame 0). Now the first "no C block" is:

| PC | routes | what |
|---|---|---|
| $008DFC | 53 | savestate PC inside a 68k screen routine, not a block start (settle does not reach one) |
| $00818C | 39 | `VsramClear` (VDP port loop), after the 12-16 settle frames |
| $011F58 | 2 | `VdpQueueVScroll`, frame 194-195 |
| $000200 | 2 | `Reset` (cold boot, coldboot/ringstate routes): boot code is not C |
| $00871A | 1 | frame 76 |

`T2_NATIVE_SETTLE=1` (a development aid, `emu.c native_settle`) lets a state that was saved in the middle of a 68k
function run on the emulated CPU until the PC is at a C block; states made with `--hle none` cannot be loaded
otherwise. In native mode the guards that decline a call for an interrupt (`safe_run`, `head_ok`, `due`) answer
"safe", i.e. interrupts are taken between blocks; making that exact is the interrupt-inside-C task.
`T2_HLE_WATCHDOG=N` (hle.c) reports the C block that is still running after N seconds.

`port/hle/native_core.c` (the CPU API for a build without the 68k core, as Scooby's P3) is carried over but not built yet.

## Cold boot and the census-driven blocks (wave 2)

Two generators now write the C that the first wave left to the 68k. Both emit **pieces** like `piecegen.py` (an entry,
straight code up to the next `jsr`/`bsr`/`jmp`/`rts`, which the glue blocks of `glue.c` execute) and charge every original
instruction with its GPGX cost on the instruction timeline; `costcheck.py` checks the charges. The file
`tools/py/bootgen.py` differs from `piecegen.py` in what a piece may touch and how it ends:

* **Hardware.** An instruction whose effective address is outside work RAM and ROM goes through `hle_pre` .. `hle_post`
  (the sound driver's model, `game/sound.c`), so the Z80 bus request, the I/O ports and the VDP see the clock the 68k core
  would give them. The decision is made at run time from the address (`Reset` loads its port pointers from a table).
  The first VDP access of a block opens the exact session of `game/vdpfill.c` (`hle_dma_begin`, `hle_exact_session`):
  the raster line follows the CPU clock, data port writes are timed (FIFO stalls) and the status word has its bits 10-15
  from the instruction prefetch. `block_end()` closes it with `hle_vdp_end()`, which, unlike `hle_dma_end()`, does **not**
  complete a DMA that is still running: `Reset` starts a 64 KiB VRAM fill that the original lets run on into frame 1.
* **Loops.** A backward branch is an exit of the piece: one pass of a loop is one block (a few dozen cycles). `Reset`
  clears 64 KiB of work RAM with a `move.l d0,-(a6) / dbra` loop of 3.6 frames; as one call it could never run in the
  emulated build (a C call must not straddle the frame end), and it would be unexact against the interrupt checks. As
  16384 one-pass blocks the layer hands any single pass to the 68k when an event falls in it (the "frame end" declines of
  `bt_Boot_00025C` in `T2_HLE_DECLINES=1`), and the rest runs as C.
* **SR writes.** A write to SR ends the piece (the new mask may take a pending interrupt before the next instruction).
* **Other.** `move usp`, `move sr`, `movem` to and from memory, `abcd`/`sbcd` (GPGX's undefined N/V included), `mulu`/`muls`/
  `divu.w` (GPGX's data-dependent cycles), `adda`/`suba`, `eor`, `rol`/`roxl`, `(d8,pc,Xn)` operands.

`port/game/boot.c` (`bootgen.py`, a fixed list) is the cold boot: `Reset` ($000200) including the Z80 handshake, the PSG
and VDP initialisation and the work-RAM clear, `Boot` ($007406), the sound driver's Z80 boot code (`sub_0FBB00`,
`sub_0FC32E/350/3E6/438`: reset, loader stub, 5120-byte driver upload), `InitPort1`, `ReadPort2Id`, `VdpSetRegs`,
`CramClear`, `VsramClear`, `sub_0081B6`, `VramClear`/`VramClearAlt` (which jump into the fill loops of `vdpfill.c`).
The pieces were validated by the lockstep gates, not by `dt`: a cold boot has no savestate to capture an entry from.

`port/game/auto2.c` (`bootgen.py auto`) covers the addresses a native run reaches from a C block with no block there:
the native runner with **`T2_NATIVE_FALLBACK=1`** (a census aid, never a gate) executes such an address on the 68k core,
one instruction, and lists it at exit as `native-fallback: $ADDR COUNT ENTRIES` (ENTRIES: reached from a C block, i.e. an
entry the C has to cover; `native.c`). One run of a route reports everything it needs, not only the first stop.
`tools/py/nativecensus.py` merges the logs, groups them by function of `data/functions.txt` and writes
`runs/native/entries.txt`; `bootgen.py auto ENTRIES REGISTERED SITES` generates one function of pieces per data/functions
entry (every entry of it, so it also covers what the function falls into) and lists the `jsr`/`bsr`/`jmp`/`rts` it stops
at, which go into `data/glue_sites.txt` for `gluegen.py`. Loop it until the census is empty:

```
OUT=runs/native T2_NATIVE=1 T2_NATIVE_FALLBACK=1 port/tools/ipprof.sh     # on a VM
python3 -I tools/py/nativecensus.py runs/native/work                      # table + runs/native/entries.txt
python3 -I tools/py/bootgen.py auto ENTRIES_ALL REGISTERED SITES > port/game/auto2.c   # see the header of bootgen.py auto()
python3 -I tools/py/gluegen.py > port/game/glue.c
```

The `$008DFC` states (53 routes) needed no settle in the end: the `rts` there was one of 34 barrier instructions between
C pieces without a glue block (the first wave's census predates the last `gluegen.py --add`). With the glue sites added
(`data/glue_sites.txt` grew by the barriers of the census-listed functions and the exits of `boot.c` / `auto2.c`) every
state starts natively; `T2_NATIVE_SETTLE` is no longer needed.

### Results (wave 2)

Native runner, strict (`OUT=runs/native T2_NATIVE=1 port/tools/ipprof.sh`, 97 runs, no fallback):

| route | frames | first stop |
|---|---|---|
| coldboot_gate | 13 | $00800C VramClearDma |
| ringstate_gate | 13 | $00800C VramClearDma |
| pause_gate | 13 | $00800C VramClearDma |
| levelflow_gate | 13 | $00800C VramClearDma |
| screens_gate | 13 | $00800C VramClearDma |

States (92 runs of 3000 frames): 50 ran to the end with no 68k instruction.

| first stop | runs | frames before it (min / median / max) |
|---|---|---|
| $00800C VramClearDma | 39 | 13 / 15 / 1912 |
| $011FA2 DmaToVram | 3 | 77 / 742 / 1400 |

Census with the fallback (`T2_NATIVE_FALLBACK=1`, the same 97 runs, 423,301 frames): **64 distinct addresses** have no
C block (was 5953 interpreted in the emulated census, 1052 reached natively before the generators), 13.6 M instructions
run on the 68k core (2.57 G before, of which 2.50 G were the wait spins, now idle blocks): all of them in `VramClearDma`
(17 addresses, 13.37 M instructions) and `DmaToVram` (47 addresses,
0.21 M). Emulated census (`--hle all`, the same 97 runs): 4937 interpreted addresses (was 5953), 2,550,293,707 top-level
instructions (was 2,573,501,147; 2.50 G of them are the wait spins); docs/remaining68k.md.

Verification with `T2_NATIVE` unset (the new blocks are active in the emulated build, so the gates prove their cycles):
`scriptgate.py` coldboot_gate 9000 (also with `T2_HASH_Z80=1`), ringstate_gate 53768, pause_gate 5738, levelflow_gate
43913, screens_gate 34882 frames identical; `lockstep.sh --frames 3000` and `--z80` all six states identical;
`/opt/t2/bin/wide.sh` 92/92; `mousetest.sh` (with `--hle all`) ALL PASS; `make` OK; `romscan.py --tracked` CLEAN.
`dt` (state, registers, VDP, Z80, I/O writes) where a capture route reaches the entry: `VdpQueueVScroll` 200/200,
`sub_0122CA` 12/12, `CramClear` 1/1, `VsramClear` 3/3, `VdpSetRegs` 3/3, `HScrollClear` 3/3, `sub_007FA4` 200/200; the cold
boot, `InitPort1` and the Z80 loader have no capture route (no savestate exists before `Reset`): the lockstep gates are the proof.

Not done / left: the two DMA routines (`VramClearDma`, `DmaToVram`: the interrupt-inside-C task); the native runner does
not yet start a route cold past `VramClearDma` (frame 13), so "coldboot/ringstate run natively" holds up to there
(`Reset`, `Boot`, the Z80 driver upload and the first VDP clears are C; with the fallback the whole 9000 frames run and only
`VramClearDma` is interpreted).
