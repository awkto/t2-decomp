# t2port: the native SDL2 host

`t2port` runs the original ROM on a Genesis Plus GX core that is compiled into the program (no
libretro `.so`, no dlopen). The mouse is a Sega Menacer light gun on port 2: the game supports it
natively ("START 1 PLAYER - USE MENACER", the default menu entry), so aim and fire need no ROM
change. The game draws its own crosshair; the host hides the OS cursor over the picture.

## Build

Needs a C compiler, CMake, `libsdl2-dev` and a Genesis-Plus-GX git checkout in `ext/gpgx`
(`git clone https://github.com/libretro/Genesis-Plus-GX ext/gpgx`; `-DGPGX_DIR=...` points
elsewhere, `-DGPGX_CLONE=ON` clones it once). From the repo root:

    cmake -S port -B build/port -DCMAKE_BUILD_TYPE=Release && cmake --build build/port -j4

At configure time CMake runs `git archive` for the pinned commit (`60bee4c`) into
`build/port/gpgx-src` and compiles that copy, so `ext/gpgx` is never modified. No core patches are
needed by the host. Output: `build/port/t2port`.

Genesis Plus GX is licensed for non-commercial use only (`port/third_party/gpgx/LICENSE.txt`);
binaries built from this port must not be sold or used commercially.

## The ROM

You need your own dump, sha1 `85cc1cf3379d3ce23ca3c03d84fe6e2b3adc9c56` (raw, non-interleaved
`.md`/`.bin`). An interleaved `.smd` dump (512-byte header) is accepted too: it is converted in
memory and the sha1 of the result is checked. Anything else is refused. Lookup order: `--rom`,
`rom =` in the ini, a ROM next to the binary, `rom/baserom.md` in the working directory (and, in a
development build, in the source tree), then a file picker (zenity/kdialog) or a "drop the ROM on
the window" screen. A ROM that worked is remembered in the ini.

## Controls

| Input | Effect |
|---|---|
| Mouse position | Menacer aim (the game's crosshair follows) |
| Left button | Trigger (Menacer A) |
| Right button | Menacer button A, B or C, option `mouse_right` (default B) |
| Middle button | Menacer Start (skips the intro text, pauses) |
| Mouse outside the picture | The gun points off screen |
| Arrows, Z/A, X/S, C/D, Enter | Pad on port 1: D-pad, A, B, C, Start (menus, 2 PLAYER pad) |
| Gamepad | D-pad / left stick, X/Y = A, A = B, B/RB = C, Start; Back = pause |
| F1 | Debug overlay (frame, FPS, gun position and buttons, pad, audio queue) |
| F5 / F9, F6 / F7 | Save / load quick state, previous / next slot (0-9) |
| P, N or `.` | Pause, frame advance |
| Tab (hold) | Fast forward |
| F11, Alt+Enter | Fullscreen |
| F12 | Screenshot to `runs/port/shot-FRAME.ppm` |
| Esc | Quit |

The picture is drawn centred at the largest whole-number scale that fits the window; the mouse is
mapped through exactly that placement. `--no-mouse` (or `mouse = off`) leaves port 2 empty.

## Options

`--options K=V,K=V` for one run, `~/.config/t2-decomp/t2.ini` (or `--config FILE`) for permanent
ones; `--options --json` lists all of them. Precedence: default, ini, command line.

| Option | Default | Meaning |
|---|---|---|
| `scale` | 3 | initial window size, multiple of 320x224 |
| `fullscreen` | off | start fullscreen |
| `vsync` | on | sync to the display |
| `mouse` | on | Menacer on port 2 (off: empty port) |
| `mouse_right` | B | Menacer button on the right mouse button: A, B or C |
| `mute`, `volume` | off, 100 | audio |
| `rom` | | ROM path, remembered automatically |
| `key_up` ... `key_start` | | SDL key names, several joined with `\|` |

Command line: `--rom`, `--options`, `--config`, `--scale`, `--fullscreen`, `--no-vsync`, `--mute`,
`--no-mouse`, `--overlay`, `--script`, `--realtime-script`, `--frames N`, `--screenshot FILE.ppm`,
`--window-shot FILE.ppm`, `--outdir`, `--savedir` (quick states; default `~/.local/share/t2-decomp`),
`--dump-ram`, `--no-throttle`, `--help`.

## Headless scripts

    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy build/port/t2port --script s.txt --outdir build/x --mute

Scripts run unthrottled and the program exits when the script ends (unless `--frames` follows).
One command per line, `#` starts a comment. Mouse coordinates are game pixels (0..319, 0..223).

| Command | Effect |
|---|---|
| `run N` | emulate N frames with the held pad buttons |
| `press BTNS N` | hold pad buttons (letters `UDLRABCS`) for N frames, then release |
| `hold BTNS` / `hold -` | set / clear pad buttons held during `run` |
| `mouse X Y` | aim the Menacer at game pixel X,Y |
| `mouse leave` | point it off screen |
| `click L\|R\|M [N]` | hold the left (trigger) / right / middle (Start) button for N frames (default 6) |
| `shot NAME` | write `OUTDIR/NAME.ppm` (a full path if NAME contains `/`) |
| `ram NAME` | write the 64 KiB work RAM, 68k byte order |
| `save NAME` / `load NAME` | savestate `OUTDIR/NAME.state` |
| `peek LABEL ADDR [W]`, `poke ADDR W VAL` | read / write work RAM (hex, big-endian) |
| `echo TEXT` | stderr and `OUTDIR/peek.log` |

Harness scripts (`tools/harness/scripts`) also run, the harness-only `trace` being ignored.

## Tests

- `port/tools/aimtest.sh` runs `port/scripts/aimtest.txt` headless: boots, picks "USE MENACER",
  skips the intro, aims at four points and off screen, fires, and writes PPM and PNG
  screenshots to `build/aimtest/`. Open the PNGs: the crosshair must sit on each commanded
  point, and the score (top left) rises after left clicks.
- `port/tools/livetest.sh` (needs xvfb-run and xdotool) moves a real X pointer over a real window
  and checks the window-to-game mapping and trigger through F12 (printed gun position).

## How the gun reaches the core

`emu.c` calls the libretro entry points directly. After loading the game it sets port 2 to
`RETRO_DEVICE_MENACER`, and answers the core option `genesis_plus_gx_gun_input` with `lightgun`
(the default is `touchscreen`, which only has the trigger and fakes B/C/Start with touch counts).
In lightgun mode the core asks `RETRO_DEVICE_LIGHTGUN` for IS_OFFSCREEN, SCREEN_X/Y (scaled over
the viewport, so a game pixel is passed as `(2p+1)/(2w)` of the range), TRIGGER (= Menacer A),
AUX_A (= B), AUX_B (= C) and START. The core's own gun cursor is off.
