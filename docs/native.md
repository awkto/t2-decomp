# The native runner: no 68000 emulation (GitLab t2-decomp#11, phase P5)

Goal: run T2 with no 68k instruction emulation, as the sibling project Scooby does (`scooby-decomp/docs/native.md`).
The game logic is the C of `port/game/` (the HLE functions, ranges and glue blocks), run by address by a small
dispatcher, the *native runner* (`port/hle/native.c`). The rest of the machine stays GPGX's (VDP, Z80 with the
YM2612/PSG, the pads, the interrupt lines), and work RAM is still GPGX's 64 KiB array.

Status: the runner and the census exist; the C does not yet cover every 68k address the routes reach, so the
runner stops with `no C block at $ADDR` where it runs out of C (numbers below and in docs/remaining68k.md).

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

## Results (end of the infrastructure step)

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
