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

    build/port/t2port --hle none            # pure emulation (default)
    build/port/t2port --hle all             # every registered replacement
    build/port/t2port --hle all,-Random     # everything except one function
    build/port/t2port --options hle=all     # same, through the ini/option system

The registry is empty except for `Random` ($00881C) for now. `hle` is a normal option (default
`none`), so it can also go in `~/.config/t2-decomp/t2.ini`.

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
(run each state once per variant), `-v`, `--keep-going`. `difftest list` prints the registry,
`difftest check all` tests everything.

## The capture route

`port/scripts/capture.txt` loads a harness savestate (`states/gun_05.state`, not committed) and
plays ~6800 frames with the mouse, which calls `Random` ~210 times. `dt` first counts the calls
on the route and keeps every k-th so the captured states are spread out, then captures 200.

## Result so far

| Address | Name | File | difftest |
| --- | --- | --- | --- |
| $00881C | Random | game/random.c | 200/200 states equal (a deliberate off-by-one is caught) |
