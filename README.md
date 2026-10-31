# T2: The Arcade Game (Genesis): mouse controls, disassembly and port

Work in progress. Goal: play *T2: The Arcade Game* (Sega Genesis, Probe Software / Acclaim, 1992)
with a mouse, natively and in a browser, built on a matching disassembly of the game.

**No game data is in this repository.** You need your own ROM: `Terminator 2 - The Arcade Game (JUE)`,
raw (non-interleaved) sha1 `85cc1cf3379d3ce23ca3c03d84fe6e2b3adc9c56`. An `.smd` dump converts with
`python3 -I tools/py/smd2bin.py IN.smd rom/baserom.md`.

## Why the mouse is easy here
The game supports the Sega Menacer light gun ("START 1 PLAYER - USE MENACER" in the title menu).
Genesis Plus GX emulates the Menacer on port 2. Feeding it the mouse position gives exact aim with
the game's own crosshair, and no change to the game. See `PLAN.md`.

## Play it
Build the native host (needs CMake, SDL2 dev files, and a Genesis-Plus-GX checkout in `ext/gpgx`) and run it:

    cmake -S port -B build/port -DCMAKE_BUILD_TYPE=Release && cmake --build build/port -j4
    build/port/t2port --rom path/to/your.rom        # raw .md/.bin or interleaved .smd, sha1 checked

Pick "START 1 PLAYER - USE MENACER" (the default) at the title menu: the mouse aims the game's
own crosshair, left click fires, right click is a second Menacer button (`mouse_right=A|B|C`),
middle click is Start. Keyboard and gamepad drive the normal pad on port 1. Headless scripting
(`mouse X Y`, `click L|R|M [N]`) and all options are in `docs/port.md`.

## Status
`make` rebuilds a byte-identical ROM from the disassembly (`sha1sum -c` against the sha1 above). The disassembly is
guided by runtime coverage from a first pass (attract mode, a Menacer game, a joypad game), so it is complete only for the
first stage and what the 68k reaches around it; see `docs/coverage.md`. Method and tools are those of the sister project
scooby-decomp.

## Layout
- `port/` - `t2port`, the SDL2 host with Genesis Plus GX linked in; `port/tools/aimtest.sh` is its headless aim/fire test
- `tools/py/ppm2png.py` - PPM to PNG for screenshots (standard library only)
- `rom/` - copy your ROM to `rom/baserom.md` (not committed)
- `src/t2.s` - generated disassembly; every non-code range is `.incbin "baserom.md"` from your ROM, so no ROM bytes are committed
- `Makefile` - `make` (rebuild + sha1 check), `make disasm` (regenerate `src/t2.s`), `make functions` (also regenerates
  `data/functions.txt`, `callgraph.txt`, `xrefs.txt` and checks that every executed instruction is in exactly one function),
  `make coverage` (`docs/coverage.md`)
- `tools/py/disasm.py` - recursive-descent disassembler: vectors + `data/exec_starts.txt` + gap fill + indirect-jump resolution
  (`tools/py/indirect.py`: jump/pointer tables, `jmp (T,pc,dX.w)` into `bra.w` tables, RAM handler vectors); extra seeds in
  `tools/py/seeds.txt`; project constants in `tools/py/project.py`; the 68k code area in `data/code_regions.txt`
- `tools/py/symtab.py`, `tools/py/xref.py` - function partition, call graph, xrefs; `xref.py ADDR|NAME` lists callers and RAM readers/writers, `--top`
- `tools/py/mergecov.py`, `tools/py/covreport.py` - merge harness `cov.bin` files into `data/exec_starts.txt` and `data/rom_data_reads.txt`; coverage report
- `tools/py/fuzzscript.py`, `tools/py/campaign.py`, `tools/py/vmjob.sh` - random-play harness scripts (`attract`, `gun`, `pad`) and a coverage campaign driver
- `tools/harness/` - headless libretro harness (patched GPGX core) with Menacer support (`GUN=1`, `gun X Y BTN N`); `h.sh` runs a script, `shot2png.sh` converts screenshots
- `data/` - runtime coverage; hand-edited `symbols.txt` (ROM labels), optional `ram.txt` (RAM/I-O vars, emitted as `.equ`),
  `gap_notes.txt` (classification of non-code ranges), `code_regions.txt`; generated `functions.txt`, `callgraph.txt`,
  `xrefs.txt`, `indirect.txt`, `code_gaps.txt`
- The Z80 sound driver is a $1400-byte blob at $0FC5CC (copied to Z80 RAM $0C00). It is `.incbin`'d for now; no Z80 disassembly yet
- `tools/vm/` - helpers to farm heavy builds/tests out to remote Linux machines
- `tools/py/romscan.py` - proves no ROM bytes are committed (`romscan.py --tracked`)
- `PLAN.md` - phases and findings

## Setup
    sudo apt install binutils-m68k-linux-gnu
    python3 -m venv .venv && .venv/bin/pip install capstone
    make functions && make            # needs rom/baserom.md; .smd dumps: tools/py/smd2bin.py IN.smd rom/baserom.md

Extending coverage needs the harness (Genesis Plus GX with `tools/patches/gpgx-cpuhook.patch`, built with `HOOK_CPU=1`, in `ext/gpgx`):

    gcc -O2 -I ext/gpgx/libretro/libretro-common/include -o tools/harness/harness tools/harness/harness.c -ldl
    .venv/bin/python -I tools/py/campaign.py run gun --first 1 --seeds 8 --frames 30000   # also: pad, attract
    .venv/bin/python -I tools/py/mergecov.py runs/cov_acc.bin
    make functions && make coverage

Runs land under `runs/` (gitignored); use a VM (`tools/vm/`) for long campaigns.

## License
MIT (see `LICENSE`) for this repository's own code. `t2port` links **Genesis Plus GX** (compiled from your own
checkout, licence text in `port/third_party/gpgx/LICENSE.txt`), which is licensed for **non-commercial use only**:
binaries built from the port may not be sold or used commercially. SDL2 is linked dynamically (zlib licence). *Terminator 2* and *T2: The Arcade Game* are trademarks of their owners. This project is not affiliated
with Acclaim, Probe, Midway, Carolco or Sega, includes none of the game's data, and requires you to own the cartridge/ROM.
