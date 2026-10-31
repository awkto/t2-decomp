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

## Layout (so far)
- `port/` - `t2port`, the SDL2 host with Genesis Plus GX linked in; `port/tools/aimtest.sh` is its headless aim/fire test
- `tools/harness/` - headless libretro harness (patched GPGX core) with Menacer support (`GUN=1`, `gun X Y BTN N`)
- `tools/vm/` - helpers to farm heavy builds/tests out to remote Linux machines
- `tools/py/ppm2png.py` - PPM to PNG for screenshots (standard library only)
- `tools/py/romscan.py` - proves no ROM bytes are committed
- `PLAN.md` - phases and findings

## License
MIT (see `LICENSE`) for this repository's own code. `t2port` links **Genesis Plus GX** (compiled from your own
checkout, licence text in `port/third_party/gpgx/LICENSE.txt`), which is licensed for **non-commercial use only**:
binaries built from the port may not be sold or used commercially. SDL2 is linked dynamically (zlib licence). *Terminator 2* and *T2: The Arcade Game* are trademarks of their owners. This project is not affiliated
with Acclaim, Probe, Midway, Carolco or Sega, includes none of the game's data, and requires you to own the cartridge/ROM.
