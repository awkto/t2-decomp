# High frame rate (GitLab github/t2-decomp#14)

Code: `port/src/render/hifps.c/.h` (snapshots, sprite owners, matching, the in-between pictures, counters),
three small hooks in `port/src/render/render_emu.c` (line 0 / every line / last line / frame end), the present
clock and the pointer in `port/src/main.c` (desktop) and `port/src/web/main_web.c` (browser). Test:
`port/tools/hifpstest.sh`. The design is that of the sibling project scooby-decomp (`docs/subsystems/hifps.md`),
adapted to T2's VDP use: cell-row parallax instead of one camera, sprites announced by a queue in work RAM
instead of by the game code, and a game that often overruns its frame.

## Using it

```
build/port/t2port --fps 120          # or --options fps=144, or  fps = 144  in t2.ini
build/port/t2port --fps auto         # the display's refresh rate, if over 60 Hz
F4                                   # on / off while playing (the option's rate, else the display's, else 120)
```

Browser: `index.html?fps=on` (or `?fps=off`), or the "Frame rate" box in the page header (kept in the ini).

- The game logic stays at 59.92 Hz; only what is *drawn* and *when it is presented* changes. The machine state
  is identical to the 60 Hz run frame for frame (below). It is **off by default**; with it off nothing changes
  (every gate is identical).
- It needs the C renderer's scene (`docs/render.md`): `fps` selects `--render c` if you did not.
- Scripted / batch runs (`--script`, `--frames`) stay at 60 Hz unless they pass `--fps` on the command line; such
  runs draw (and count) every in-between right away and present nothing ("eager", used by the tests).
- With vsync on, a rate above the display's refresh is capped to it (every flip waits for a refresh).

## How a picture is made

For logic frame k the C renderer draws the whole frame R(k) in raster mode as before. `hifps.c` keeps a copy of
the VDP state it was drawn from (VRAM, CRAM, VSRAM, registers, sprite cache) and draws the pictures *between*
R(k-1) and R(k) with the C renderer's frame mode (`render_frame`) from that copy, with the scroll values and the
sprite positions moved back toward R(k-1): the picture at fraction f of the step (0 < f < 1) shows the scene at
(k-1) + f. `f = 1` is R(k) itself.

| what | how | snaps / holds |
|---|---|---|
| **per-row h-scroll** (both planes; the stages scroll by cell-row parallax, `docs/subsystems/vdp.md`) | every table entry is moved on its own by `round(step x (1-f))` | a step over 64 px (a cut) holds the frame |
| **v-scroll** | the same, per VSRAM word | step over 64 px |
| **sprites** | the objects the previous frame also drew, matched through the sprite queue (below); every piece of the object moves by the object's step | moved further than 40 px = a different object, not moved |
| **the crosshair** | drawn at the pointer *now* in every picture (below) | |
| not moved | palettes, tiles, animation frames, the HUD rows (their scroll step is 0) | |

The in-betweens are whole pixels (the game's resolution): a step of 1 px between two frames cannot be split, so
at 120 Hz it appears in the first or the second picture (the rounding is half away from zero, so the earlier
picture keeps the old position).

### Sprite owners and matching (no change to the game code)

`BuildSpriteTable` ($013272) leaves its queue in work RAM: `spr_queue_n` $FF9DF2, the (index, priority) pairs
`spr_queue` $FFA250 (bubble-sorted by priority, the order of the SAT) and the arguments `spr_queue_args` $FFA318 (frame id,
X, Y, priority). `hifps.c` reads it and rebuilds the pieces of each entry from `SpriteMappings` ($0152A8): the
queue **explains** the sprite table only if every piece's Y, X and size agree and the table equals the VDP's copy
word for word. Only then are the entries the objects of the frame. The queue is read at the end of each display
(when the game has finished building the next frame), two displays back, at line 0 and again at the end; whichever
explains the table drawn is used (counters in the `hifps:` report). The same works with the 68k `BuildSpriteTable`
(`--hle none`) and the C one.

An entry of R(k) is matched to an unmatched entry of R(k-1) with the lowest cost
`|dx| + |dy| + 12 (other frame id) + 6 (other piece count)`, within 40 px on both axes; crosshair entries (frame
ids 1 and 2) match only each other. There is no object slot in the queue (QueueSprite is called with numbers), so
this is a heuristic. Unmatched entries (new objects) are drawn at their R(k) position.

### The crosshair

The Menacer crosshair is a game sprite (player 1: frame id 1, `QueueCrosshair`, `docs/gun.md`). In every picture
presented, whole frames included, it is moved by (pointer now) - (the pointer sample the game drew it at): the
sample is the nearest of the last four positions given to the game, and only if the crosshair is within 16 px of
it (so it follows the mouse only when the game really follows it: not with a pad-driven crosshair, not when the
Y clamp 24..200 bites). The game still receives the mouse at its 60 Hz frame; the *picture* of the crosshair moves
at the display rate. The shot lands where the game's crosshair is (its position at the frame), as before.
A whole frame with a moved crosshair is drawn in frame mode (only when the frame is reproducible, below).

## Frames that overran: what the picture is made from

T2's loop (`docs/subsystems/mainloop.md`) runs the VDP list (SAT, h-scroll, tilemap columns, palettes) after the
VBlank wait. When the logic of a frame takes longer than a frame (heavy scenes, level 4 and 8 here), the list runs
*during the display*, a few to a hundred lines in, and the raster picture is part old, part new. A snapshot taken at
line 0 would then be the old picture. So:

- Every line the live VDP is compared with the snapshot (h-scroll table, palette, SAT at each sprite parse,
  VSRAM, registers; the DMA-enable bit of register 1, which the game flips around every DMA, is ignored).
- If the sprite table or the h-scroll table changed within the first 112 lines, the picture is taken from the VDP
  state at the **end** of the display instead (`adopted` in the counters): nearly all lines were drawn from it. The
  first lines differ from it where sprites changed; they are mended from the raster frame (below).
- An h-scroll table changed later is composed per table entry (entries from the new table below the change line).
- A sprite table changed after line 112, a v-scroll or a register changed during the display: the frame is held.

### The check, and the lines taken from the raster frame

Before a frame is used its snapshot is drawn in frame mode with no motion and compared with the raster frame. Lines
that differ (the HUD rows, written directly to VRAM during the display; the first lines of an adopted frame; a
tile-art load during the display) are remembered and **copied from the raster frame** into every in-between: they
do not move, the rest does. More than 80 differing lines, and the frame is not used (held).

## Frames that are not interpolated

Shown as at 60 Hz: the in-between pictures are R(k-1) (with the crosshair at the pointer) until R(k) is due. Counted
by reason in the `hifps:` report: no previous frame / state load (`emu_state_load` calls `hifps_reset`); layout
changed (registers 0-5, 11-13, 16-18, size); frame mode differs (check above); owners unknown (the sprite table
changed and no queue explains it); scene cut; changed during the display. Frames where nothing moved are *still*
(the commonest in menus and when the player stands).

## Timing (desktop)

T = 1 / 59.92 s. With rate H each logic frame shows **n = round(H x T)** pictures (120 and 144 Hz: 2, 165 and 180: 3,
240: 4). The whole frame R(k) is presented at kT, the in-betweens at (k-1)T + jT/n. Each picture is prepared (drawn,
converted, uploaded) first, then the host waits for its time and only flips, so the spacing is even. The clock
advances by exactly T a logic frame (no absolute H-Hz clock, which would beat against 59.92 Hz); with audio on it is
nudged by T/32 when the audio queue drifts a frame from its 50 ms target, and restarts when more than four frames
late. Price: R(k) is shown one frame after the 60 Hz run would show it (the first change toward it T/n after
(k-1)T) - that is what interpolating costs; extrapolating would avoid it by guessing.

Browser: `requestAnimationFrame` already fires at the display's refresh. Each callback shows the scene at the
fraction `acc / frame_ms` of the logic step that has gone by (no fixed n), so a 144 Hz display gets 2.4 pictures
per logic frame, with the pointer at its position at that callback.

## Presentation only (what the test checks)

The in-betweens are drawn from host copies (the snapshot, work VRAM / VSRAM / sprite cache); nothing is written to
the emulated machine. `port/tools/hifpstest.sh` runs every savestate for 1500 frames of the lockstep input with and
without `--fps` and requires the per-frame hashes of work RAM, VRAM, CRAM, VSRAM and the VDP registers to be
identical, and prints how many frames were interpolated.

## Diagnostics

| env | |
|---|---|
| `T2_HIFPS_STATS=1` | the `hifps:` counters at exit (always in eager runs) |
| `T2_HIFPS_DEBUG=1` | frames whose check failed (the differing lines), sprite-table / scroll / palette changes during the display, unknown owners |
| `T2_HIFPS_DUMP=DIR[,FROM-TO]` | eager runs: write every picture of frames FROM..TO as `sFRAME_J_{w,i,h}.ppm` (whole / interpolated / held) |
| `T2_HIFPS_MOUSE=DX,DY` | eager runs: the pointer is DX,DY from where the game read it (tests the crosshair) |

## Known limits

- Whole pixels: no sub-pixel placement. Animation frames and palettes keep the game's rate (an in-between shows
  R(k)'s frame at the interpolated position).
- Sprite matching is a heuristic (no object slot in the queue): two similar sprites that cross within 40 px can swap.
- Frames that overran with the list running after line 112, and v-scroll / register changes during the display, are
  held; the game itself tears there.
- Rates that are not a multiple of 59.92 Hz present n = round(H x T) pictures per frame (144 -> 119.8 a second),
  which a 144 Hz vsynced display cannot show evenly.
- Interlace and the 128 KiB VRAM mode are not supported (the game never uses them), as in the renderer.

## Results

`port/tools/hifpstest.sh` (all 92 states, 1500 frames of the lockstep input each, `--hle all`, 120 Hz, in-betweens
drawn eagerly):

```
hifpstest: 92 states, machine state identical to 60 Hz in 92
hifpstest: 138000 frames: 100164 interpolated, 36820 still, 1016 held; sprite entries matched 957404 of 978268; owners unknown on 3671 frames
hifpstest: PASS
```

- Machine state (work RAM, VRAM, CRAM, VSRAM, VDP registers, per frame) identical to the 60 Hz run in every state: it
  is presentation only. With the feature off (the default) every existing gate is identical: lockstep six default
  states plain and `--z80` (3000 frames each), the five route gates (5738 / 43913 / 34882 / 9000 / 53768 frames),
  the wide gate (92 of 92, `--hle all` 3000 frames), `rendergate` (92/92 states, 5/5 routes pixel-identical), the
  mouse tests (ALL PASS), `make` (build/t2.md: OK) and `romscan` (CLEAN).
- 72.6 % of the frames are interpolated, 26.7 % are still (nothing moved: menus, a standing player), 0.7 % held. The
  heavy scenes (level 4 and 8 states, whose frames overrun) are 98-99.9 % interpolated thanks to the end-of-display
  state; 97.9 % of the sprite entries find their object in the frame before; 2.7 % of the frames have owners that no queue
  explains (held only when the sprite table also changed).
- Cost of a picture drawn in frame mode: 0.5-0.8 ms on the VM (p99 about 1 ms, 3 ms worst; 320x224, the cost is
  the C renderer's, `render_frame`).

Real-time presentation (t2-vm2, level 4 route, 1500 frames, `--realtime-script --mute --no-vsync --hle all`,
SDL dummy video driver with its software renderer, no audio, no vsync; `--scale 1` so that the software flip
is cheap, as a GPU's is). Whole frames every 16.69 ms (T). Spacing of consecutive presents against T/n:

| rate | n | pictures a second | within 1 ms | sd | cpu (one core) |
|---|---|---|---|---|---|
| 60 (off) | 1 | 59.9 | | | 13 % |
| 120 | 2 | 119.8 | 99.7 % | 0.14 ms | 40 % |
| 240 | 4 | 239.6 | 77.5 % | 0.86 ms | 52 % |

At `--scale 3` (960x672 in software) one flip costs 2 ms on this VM, and 120 Hz is 119.8 pictures a second, 95.6 %
within 1 ms, 63 % of a core (60 Hz: 24 %); 144 Hz is also n = 2 (119.8 a second, 96.4 % within 1 ms); 240 Hz needs the CPU that is not there (237 a second, 96 % of a core): the
flip, not the interpolation, is the cost there (0.8 ms to prepare a picture, 2.0 ms to flip). The logic frame
(3 ms on this VM in the heavy level) runs between the last picture of a step and the first of the next, so on a
slow machine and at n = 4 the first in-between is late (never dropped). The interpolation's own cost is the
0.5-0.8 ms picture, once per extra picture: at 120 Hz about 5 % of a core.

Browser (`tools/vm/webtest.sh`, headless Chrome, `?hle=all&fps=on`): PASS, 59.9 logic frames a second, 0 failed, same
as without `fps`.
