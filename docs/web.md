# The browser build

`t2port` compiled to WebAssembly with Emscripten and SDL2 (Emscripten's port). It is a static site:
`index.html`, `app.js`, `style.css` (`web/`) and `t2.js` + `t2.wasm` (`port/src/web/main_web.c` with the
same `emu.c`, `gun.c`, `options.c`, HLE layer and Genesis Plus GX core as the desktop host, docs/port.md). It
holds no game data: the player provides the ROM. The mouse is a Menacer, as on the desktop.

## Building

    EMSDK=<emsdk dir> tools/web_build.sh        # -> build/web/
    cd build/web && python3 -m http.server 8000

`tools/web_build.sh` runs `emcmake cmake -S port -B build/web-cmake -DT2_DIST=ON -DGPGX_OPT=-O3` and
builds the `t2_web` target only (under Emscripten the CMake project returns after it; the desktop
program is not configured). Needs a Genesis-Plus-GX checkout in `ext/gpgx` (`GPGX_DIR=` for another
place), like the desktop build. Emscripten 6.0.13 is the pinned version (CI installs it; the script
notes a different one). No ROM is needed to build. With `rom/baserom.md` present the script runs
`tools/py/romscan.py` over `build/web/`; without it, the secret-free check (below).

Link settings (`port/CMakeLists.txt`): `-O3`, `-sUSE_SDL=2`, `-sINVOKE_RUN=0` (the page calls `main`
once it has the ROM), `-sEXIT_RUNTIME=0`, memory growth from 64 MB, a 1 MB stack, IDBFS. No ASYNCIFY:
the browser build has no blocking loop.

## C replacements (HLE)

The page runs the same HLE layer as the desktop (docs/porting.md; `port/hle`, `port/game/*.c`, 1090
replacements at the time of writing, all in the `t2_web` target, which globs `port/game/*.c`). Default `none`: the original 68k code
runs, as on the desktop. Choose the set with the `hle` option (ini line `hle = all` or
`hle = Random+MenacerReadXY`; `+` separates names; the ini is `/t2/t2.ini` in IndexedDB, read at start)
or, for one visit, the URL parameter, which wins over the ini:

    index.html?hle=all
    index.html?hle=none
    index.html?hle=Random+MenacerReadXY       # a `+` or `,` list; `-name` turns one off (all,-Random)

`app.js` passes the parameter to `main` as `argv[2]`. An unknown name is reported on the console and the
run falls back to `none`. The console prints `hle: N active`; `web_hle_count()` (exported) returns N.

## The main loop

`emscripten_set_main_loop(web_frame, 0, 1)`: the browser calls `web_frame` on every
`requestAnimationFrame`. The emulation keeps its own clock at the core's frame rate (59.92 Hz): an
accumulator of real time runs as many frames as are due (usually 0 or 1, at most 4 to catch up; a gap
over 250 ms, e.g. a hidden tab, restarts the clock instead of fast-forwarding). A picture is
presented whenever a frame ran. Tab (held) runs 4x. Audio is SDL2's Web Audio device, queued per frame;
the queue is kept near 60 ms by moving the clock by 1/32 frame when it drifts. Browsers start an
AudioContext only after a user gesture (see "The ROM").

## Mouse, exactly as the native host

The page's canvas fills the stage in CSS. The SDL window follows the canvas's CSS size every frame
(created with `SDL_WINDOW_ALLOW_HIGHDPI`, so the drawing buffer is CSS size x devicePixelRatio). SDL
reports pointer positions in CSS pixels; `window_to_game` scales them to the drawing buffer and
inverts the picture's placement: centred at the largest whole-number scale that fits (the same rule
and the same numbers as `gun_map_window` in `gun.c`; a canvas smaller than 320x224 gets the picture
fitted to it instead). The pixel goes to `mouse_move`, and `mouse_frame` hands it (X corrected, option
`aim_correct`) and the buttons to the core before every emulated frame, and returns the pad-1 presses
for the menus. The whole mouse logic (`mouse.c`, menus, continue, aim correction) is the same code as
in the native host; the in-memory aim patch (option `aim`, default `fast`) is applied in `emu.c`.
The `aim` option is read once at start (change it in the ini and reload); `aim_correct` is live.

| Input | Effect |
|---|---|
| Pointer over the picture | Menacer aim (the game draws the crosshair) |
| Pointer outside the picture (the letterbox bars) or leaving the canvas | gun off screen |
| Left button | trigger (Menacer A) |
| Middle button | Menacer Start (skips the intro text, pauses); Start on the menus |
| Right button | Menacer A, B or C: option `mouse_right`, default B (D0 = the missile); the "Right click" box in the page header sets it and keeps it in the ini. On OPTIONS: value down |
| Wheel | main menu: cursor up/down; OPTIONS: value up/down |
| Click on the logos, story text, PRESS START | Start (pad press injected by `mouse.c`) |
| Main menu: move over a row, left click | select the row, left click starts it |
| OPTIONS: move over a row; left / right click | select; value up / down; left click on EXIT leaves |
| Left click on the continue screen | the gun's Start: continue |
| Touch tap | SDL turns it into a mouse move + left click: aims and fires |

A press is held for at least 4 emulated frames, so a click that is shorter than a frame is not lost.
The browser's context menu is suppressed on the canvas, the middle button does not autoscroll, and the
CSS cursor is `none` over the canvas (the game's crosshair is the cursor). The pointer is absolute: no
pointer lock.

The keyboard is the pad on port 1 as on the desktop (the `key_*` options: arrows, Z/A, X/S, C/D, Enter);
P pause, N one frame, Tab fast forward, F1 debug overlay (gun position and buttons), F5/F9 save/load a
state, F6/F7 slot. The page stops the browser from acting on the game's keys (arrows, Tab, Enter,
F1-F10); F11 and the Fullscreen button put the stage in fullscreen. Gamepads: a pad SDL maps is a
GameController (the desktop bindings); any other is read as the browser's "standard" layout.

## Persistence (IndexedDB)

`app.js` mounts IDBFS at `/t2` before `main` and loads it (`FS.syncfs(true)`):

| path | what |
|---|---|
| `/t2/rom/baserom.md` | the player's ROM, kept after its sha1 matched |
| `/t2/t2.ini` | the options (`port/src/options.h`; the page changes `mouse_right`) |
| `/t2/saves/t2.slotN.state` | savestates (F5 / F9, slots 0-9) |

After every write the C code calls `Module.t2Sync`, which writes IndexedDB 300 ms later; the page also
syncs when the tab is hidden.

## The ROM

You need your own dump, sha1 `85cc1cf3379d3ce23ca3c03d84fe6e2b3adc9c56`: a raw `.md`/`.bin` or an
interleaved `.smd` (the native host's `emu_read_rom` converts it in memory). On start the page:

1. uses the copy in IndexedDB, if there is one, but shows **Click to play** first: SDL creates its
   AudioContext inside `callMain`, so creating it in the click handler means it starts running instead
   of suspended;
2. otherwise asks for the file (file input, or drag and drop on the box or anywhere on the page). The
   file is written to IDBFS, checked by `web_rom_check` (sha1 of the whole, de-interleaved file) and kept
   on success. The choosing was the gesture, so the game starts at once.

"Forget ROM" (shown when the cached copy is in use) deletes it and reloads. The ROM is never sent
anywhere: the page makes no network request other than its own files.

## GitHub Pages

`.github/workflows/pages.yml`: on a push to `main`, install emsdk 6.0.13, fetch Genesis Plus GX at the
pinned commit (`git fetch --depth 1` into `ext/gpgx`; `port/cmake/gpgx.cmake` then exports it as in
every build), run `tools/web_build.sh`, check the site, upload it and deploy with `actions/deploy-pages`.
While the repository is private Pages may not serve it; the workflow is kept so that it works once
it does (or on a plan that allows it).

CI has no ROM, so the byte scan (every 64-byte window of the ROM against the output) cannot run there.
`romscan.py --if-rom` does what can be checked without it: no file is the ROM (its sha1 is public) or
a Genesis ROM image ("SEGA" at `$100`). That the build itself cannot carry ROM bytes follows from the
build machine not having any; the full scan runs wherever `tools/web_build.sh` runs with `rom/baserom.md`.

## Testing

`tools/web_test.sh` serves `build/web` and runs `tools/web_test.js URL OUTDIR ROM` (puppeteer-core,
`CHROME=` a Chrome or chrome-headless-shell, `NODE_PATH` for puppeteer-core): fresh profile, the page
must ask for the ROM, the ROM goes through the file input and the game starts; a reload must show
"Click to play" (ROM from IndexedDB), the click starts the game; boot to the title menu, six Enter
presses start "1 PLAYER - USE MENACER" and run the intro, a middle click skips the intro text. Then the
mouse is moved over the canvas to 7 game pixels (placed with the same letterbox maths, from outside
the page's code) and the gun position the C side holds must equal each pixel; off the picture it must be
off screen; left, right (B, then C after the setting) and middle buttons must give the Menacer masks
1, 2/4 and 8; the context menu must be suppressed, the cursor hidden; a touch tap must aim. Any console
error fails the run. Then, from a fresh game (reload), the menu flow is done by mouse only: clicks through
the logos, story text and PRESS START (waiting for each screen with `Module._web_screen()`), hovering and
the wheel move the main menu cursor, OPTIONS credits are changed with clicks, and a left click on the
first row starts the game with no key pressed. Screenshots of the stage go to `runs/web/`: look at them, the game's crosshair must
sit on the commanded pixel and shots must fire (gunpower drops, impacts and explosions).

`tools/vm/webtest.sh` (run on a VM with emsdk 6.0.13 in `/opt/emsdk`, and node 18 with puppeteer-core 23 and chrome-headless-shell in `/opt/webtest`) builds, then runs the whole test twice: default (`hle` none) and `?hle=all` (by hand: `WEB_QUERY='?hle=all' tools/web_test.sh`). The test checks that the number of active replacements matches the URL (0 for none). `tools/vm/web_provision.sh` installs emsdk, node, puppeteer-core and chrome-headless-shell on a fresh VM.

Result after the C port grew (2026-11-08, emsdk 6.0.13, chrome-headless-shell 127): the build needed no changes;
default (0 active, 59.9 emulated frames/s) and `?hle=all` (1090 active, 60.1 frames/s) both pass, 0 failed.
`t2.wasm` 4.37 MB (846 kB gzipped), `t2.js` 156 kB (40 kB gzipped); romscan of `build/web`: CLEAN (one 67-byte
shared run, a note). The page test has no frame-hash comparison between the two runs; the lockstep gates
(docs/porting.md) are what prove `all` equals the original code.

Earlier result with the HLE layer (2026-11-04, 4-vCPU VM, emsdk 6.0.13): 33 checks, all pass, no console errors,
for default (0 active, 59.4 emulated frames/s) and for `?hle=all` (39 active, 58.9 frames/s).
`t2.wasm` 3.42 MB (703 kB gzipped), `t2.js` 156 kB; romscan of `build/web` and of the tracked files: CLEAN.

Result (4-vCPU VM, chrome-headless-shell 127, SwiftShader, emsdk 6.0.13): all checks pass, no console
errors, 59.6 emulated frames/s, game running about 2.4 s after navigation; the crosshair sat on each
commanded pixel in the screenshots (40,60; 280,180; 80,160; 240,60; touch 120,90), shots fired.
`t2.wasm` 3.29 MB (685 kB gzipped), `t2.js` 155 kB (40 kB gzipped); romscan of `build/web` with the ROM:
CLEAN. Not covered headless: sound output itself, gamepads, real fullscreen.
