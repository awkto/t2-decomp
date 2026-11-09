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
| Right button | Menacer button A, B or C, option `mouse_right` (default B = D0 = the missile) |
| Middle button | Menacer Start (skips the intro text, pauses); Start on the menus too |
| Wheel | Main menu: cursor up/down. OPTIONS: value up/down |
| Mouse outside the picture | The gun points off screen |
| Any click on the logos, story text, PRESS START | Start (pad 1 press injected by the host) |
| Main menu: move over a row, left click | Cursor to the row, then left click = Start on it |
| OPTIONS: move over a row, left click / right click | Select the row; left click = value up, right click = value down (rows 2-4 also play it); left click on EXIT leaves |
| Left click on the continue screen | The gun's Start: continue (also a dead gun player in 2 PLAYER) |
| Arrows, Z/A, X/S, C/D, Enter | Pad on port 1: D-pad, A, B, C, Start (menus, 2 PLAYER pad) |
| Gamepad | D-pad / left stick, X/Y = A, A = B, B/RB = C, Start; Back = pause |
| F1 | Debug overlay (frame, FPS, gun position and buttons, pad, audio queue) |
| F4 | High frame rate on / off (docs/hifps.md) |
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
| `aim` | fast | `fast`: the crosshair follows the mouse within a frame; `game`: the original 4-8 frame lag |
| `aim_correct` | on | host-side X correction table: the shot lands within 1-2 px of the mouse |
| `mute`, `volume` | off, 100 | audio |
| `rom` | | ROM path, remembered automatically |
| `hle` | none | run 68k functions as C: none, all, or a list of names (`+`-separated when given through `--options`, which splits on commas); see docs/porting.md |
| `widescreen` | off | wider view of the scrolling levels: off, 16:9, 21:9, `W:H` or a width; presentation only, implies `render = c` (docs/widescreen.md; `T2_WIDE` overrides) |
| `fps` | off | high frame rate (docs/hifps.md): `off`, `auto` (the display's refresh rate if over 60 Hz) or a rate in Hz (120, 144, 240). The game stays at 60 Hz; in-between pictures are interpolated. Selects the C renderer. F4 toggles |
| `key_up` ... `key_start` | | SDL key names, several joined with `\|` |

Command line: `--rom`, `--options`, `--config`, `--scale`, `--fullscreen`, `--no-vsync`, `--mute`,
`--no-mouse`, `--overlay`, `--script`, `--realtime-script`, `--frames N`, `--screenshot FILE.ppm`,
`--window-shot FILE.ppm`, `--outdir`, `--savedir` (quick states; default `~/.local/share/t2-decomp`),
`--dump-ram`, `--no-throttle`, `--hle SPEC`, `--widescreen SPEC`, `--hle-list`, `--help`.
`--dump-ram`, `--no-throttle`, `--hle SPEC`, `--fps RATE`, `--hle-list`, `--help`.

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
| `fmouse X Y` | aim at pixel X,Y of the shown frame (the wide frame with `--widescreen`; docs/widescreen.md) |
| `mouse leave` | point it off screen |
| `wheel N` | mouse wheel (N > 0 up) |
| `untilscreen NAME MAX` | run until the mouse logic sees screen `logo`, `intro`, `title`, `menu`, `options`, `continue` or `game` (at most MAX frames) |
| `until ADDR W VAL MAX` | run until the work RAM value matches (at most MAX frames) |
| `screen` | log the screen the host sees |
| `crosshair LABEL` | log gun_x, gun_y, p1_x, p1_y and the crosshair sprite's centre from the VDP sprite table |
| `cpu LABEL` | log the 68k PC, SP and the word above the top stack return slot |
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

## Mouse tests (issue #5)

`port/tools/mousetest.sh [t2port] [test ...]` runs the headless mouse tests (SDL dummy drivers, no
display, about a minute) and prints PASS or FAIL per test; PPM and PNG screenshots and the
peek logs are in `build/mousetest/<test>/`. The scripts are in `port/scripts/`, the checks in
`tools/py/mousecheck.py`. No test sends a pad command except `twop`, where the pad is the second player.

| Test | What it proves |
|---|---|
| `flow` | power-on to level 1 with clicks only: logos, story, PRESS START, main menu (hover and wheel move the cursor), OPTIONS (credits value with left/right click and wheel, EXIT), "USE MENACER" |
| `twop` | "START 2 PLAYER - 1 MENACER / 2 JOYPAD 1" from the mouse: mouse = player 1, scripted pad = player 2 |
| `continue` | energy poked to 0: left click continues (credits 4 to 3, energy refilled); dying again with the countdown run out leads to the high-score screen, where three clicks enter ABC and ENTER |
| `missile` | a 1-frame right click fires nothing (Menacer debounce), a 4-frame one fires a missile |
| `grid` | 370 points over the screen: p1_x within 2 px of the mouse (97 % within 1), p1_y exact, and the crosshair sprite agrees |
| `lag` | after a jump of the mouse the crosshair is there in 2 frames with `aim=fast` and takes 9 with `aim=game` |

`port/tools/build.sh` builds `build/port/t2port` (what the tests need). `port/tools/aimcal.sh` remeasures
the X correction table (`port/src/aimtab.h`, made by `tools/py/aimtab.py`) from the `flow` test's state.

## Menus by mouse

The game reads only pad 1 on the logos, title, main menu and OPTIONS, so `mouse.c` (SDL-free, shared
with the browser build) injects pad-1 presses. The screen is worked out from the 68k: between frames
it sits in `WaitVBlank` (`$0086E2`) and the return address on the stack names the loop that
called it (`data/ram.txt`, "host/mouse"). A press is held until the game has latched it in `$FF8050`
and released until it has seen the release, since the game wants clean edges. The main menu cursor is
`menu_cursor` (`$FF9F60`): the host presses Up/Down until it equals the row under the mouse (rows
at y = 32 + 24 n). The logo screens take Start only in a window of their 300 frames
(`vblank_count` 180..299), so a click on them waits for it. "2 JOYPAD" (row 3) is refused by the game
with a gun on port 2, so a click on it only moves the cursor. Nothing in the game is patched for this.

## Aim

`aim=fast` (default) writes `tools/py/gunpatch.py`'s 60 bytes into the core's in-memory copy of the
ROM (`$01E460`) after the sha1 check: only the newest HV latch is used, so the crosshair is where the
mouse is 2 frames later. The ROM file is never modified; `aim=game` leaves the original averaging
(4-8 frames lag). `aim_correct=on` maps the commanded X through `aim_cmd_x[]`, which is measured:
without it the game's X is -2..+4 px from the mouse, with it 0 (45 %), +1 (46 %), -1 (8 %), 2 (rare)
across x = 0..319; x = 0 gives 2 because the game's X starts at 2. Y needs no correction.

## How the gun reaches the core

`emu.c` calls the libretro entry points directly. After loading the game it sets port 2 to
`RETRO_DEVICE_MENACER`, and answers the core option `genesis_plus_gx_gun_input` with `lightgun`
(the default is `touchscreen`, which only has the trigger and fakes B/C/Start with touch counts).
In lightgun mode the core asks `RETRO_DEVICE_LIGHTGUN` for IS_OFFSCREEN, SCREEN_X/Y (scaled over
the viewport, so a game pixel is passed as `(2p+1)/(2w)` of the range), TRIGGER (= Menacer A),
AUX_A (= B), AUX_B (= C) and START. The core's own gun cursor is off.
