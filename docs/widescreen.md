# Widescreen (GitLab github/t2-decomp#13): a wider view of the scrolling levels, presentation only

Code: `port/src/render/render_wide.c/.h` (what is shown wide, the margins, the hooks, counters),
`port/src/render/render.c` (the wide line: `render_vdp.out_w / xbias / wide_hist`, sprite placement),
`port/src/render/render_emu.c` (three hook calls), `port/src/mouse.c` (`mouse_move_frame`: margins),
`port/src/emu.c` (`emu_video` returns the wide frame), `web/` + `port/src/web/main_web.c` (the page).
Tests: `port/tools/widetest.sh` (`mouse`, `state`, `shots`, `stats`), `tools/vm/webtest.sh` (the page).
The model is the sibling project's widescreen (scooby-decomp `docs/subsystems/widescreen.md`); the
difference is the game: T2 scrolls by streaming a map into 64-cell planes, so the wide view is built from
what the planes hold, and the margins need a rule (below).

## Using it

```
build/port/t2port --widescreen 16:9            # 400 x 224: 40 pixels more on each side
build/port/t2port --widescreen 21:9            # 480 x 224 (capped, see "Width")
build/port/t2port --options widescreen=16:9    # the same; `widescreen = 16:9` in t2.ini
T2_WIDE=16:9 port/tools/lockstep.sh            # the environment variable wins over the option (tests use it)
```

`widescreen` is `off` (default), `16:9`, `21:9`, `W:H`, or a width in pixels (320..640, rounded up to a
multiple of 16). It is **off by default** in the window, in scripts and in every test, so nothing existing
changes: `--screenshot`, `shot`, `--framehash`, the lockstep, render and route gates see exactly what they
saw. When on, the C renderer draws the picture (`--render c` is implied) and `emu_video()` returns the wide
frame, so the window, `--screenshot` and script `shot` show it. Script commands for it:
`fmouse X Y` = a pixel of the shown frame (the window's mouse; `mouse X Y` stays in game pixels).
Web: `index.html?wide=16:9` for one visit, or the "Widescreen" menu in the header (kept in the ini,
applied after a reload); `?hle=all&wide=16:9` combine.

## Presentation only

The game is not told anything and nothing writes to the emulated machine. The renderer draws a second,
wider line next to every normal line, from the same live VDP state at the same moments (the raster hooks of
docs/render.md), with the same sprite list and the same per-line limits. The 4:3 frame is still drawn
(`--render compare` is unchanged). Consequences:

- Game state is the machine's own: with widescreen on, the per-frame hashes of work RAM, VRAM, CRAM, VSRAM,
  VDP registers and the cycle count are identical to the run with it off (`widetest.sh state`).
- The 320 pixels in the middle of every wide line are exactly the 4:3 line (counter `centre`, 0 in every test).
- What the game itself decides stays as it is: it spawns and removes objects by its own x ranges (the level
  script spawns at X = `$154` / `$168` = 340 / 360, i.e. inside a 16:9 margin: an enemy may appear 20..40
  pixels inside the wide view, where in 4:3 it appears at the edge), and it never moves anything in a margin out
  of the way. Hit tests and the crosshair are the game's (see "The gun").

## Width

16:9 at 224 lines is 398.2 pixels, drawn as **400** (margin 40 = 5 cells each side; the width is a multiple of
16 because the VDP draws 2-cell columns). 21:9 would be 523: the planes are 64 cells = 512 pixels, the 4:3
picture needs 41 of them and each margin cells of its own, so the margins together can be at most 22 cells:
widths above **480** are capped (`21:9` gives 480, 2.14:1). The line buffers go to 640 for tests.

## What is shown wide

A frame is wide when the VDP is in the gameplay layout and a level is running: H40, planes 64 x 32 cells,
per-cell h-scroll with a full-screen v-scroll (reg 11 = `$0A`), no window, and the `level` word (`$FF80CC`)
is 1..8. That covers the intro text over the level background, the level, bosses, "Terminated", pause, and
the attract demo (levels 1..3). Everything else is a 320-pixel screen and is **pillarboxed**: the normal
frame centred in the same-size output with black sides, so the window never changes size and the mouse
mapping is constant:

| screen | why not wide |
|---|---|
| logos, story intro, title, main menu, OPTIONS (level word 0) | 320-pixel pictures (H40 text / art), no scrolling planes |
| level summary, "casualties" and score screens | H32 (256 px) layout, different planes |
| high-score initials (level word `$0A`) | its own screen |
| a frame with the left-column blank bit set, a window, or per-column v-scroll | not in T2's gameplay; the margins would be wrong, so the rule falls back |

The HUD (the bars at the top and bottom, rows 0-1 and 26-27) is drawn by the game into the planes at the 4:3
position and stays there; its margins are the planes' own cells there, which are black (screenshots).
No line of a wide frame is pillarboxed: the HUD rows and the room are one picture.

## The planes and the margins

T2 scrolls by cell-row parallax: every 8-line row of plane A and B has its own h-scroll, and `BgStreamLayers`
streams the level's map into the 64 x 32-cell planes (docs/formats.md): when a layer's position passes a
trigger it writes the next 8- or 16-cell block into the cells that just scrolled off the **left**, i.e. data
for 56..63 cells ahead. Levels 3, 7, 8 have no streamer: the planes are a 512-pixel loop. With p = the cell's
distance in cells from the picture's first cell (p 0..40 are in the 4:3 picture):

- **right margin** (p 41..46): shown as the plane holds it. The streamer is 8 or more cells ahead there, so it
  is already the level's data. This is checked on every wide frame (`stale`: a cell shown in the margin that
  later changes before it enters the picture): zero for the streamed backgrounds; what is counted are tiles the
  game draws in place (text boxes, effects), see the results.
- **left margin** (p 58..63): the cells that just scrolled off are overwritten by the streamer with data for
  the far right, so the plane there is the *future*, wrapped (a clear jump in the picture). Instead each cell
  is shown as it was the last time it was in the 4:3 picture (`g_last`, kept per plane / row / cell, reset by
  a level change or a pillarboxed frame), which is what scrolled off. Where it was never in the picture (the
  first frames of a level; the unseen cells of the looping levels) the plane is shown as it is.
  `T2_WIDE_NOHIST=1` turns this off to compare.
- The renderer decides per line, from that line's own h-scroll, whether a cell is in the left margin (so a scroll
  change in the middle of the frame, which the game makes while it loads a level, never touches the 4:3 part).
- v-scroll is full-screen in the gameplay layout; levels 3 and 7 move it by a few pixels (shake, bounce), so a
  name-table row can be on two screen rows with different h-scrolls: such a cell is treated as in the picture if
  it is in either.

## Sprites

Selection, the 16/20 per-line and 256/320-pixel limits and the x = 0 masking are those of the 4:3 machine (the
wide pass uses the 4:3 pass's sprite lists), so sprites in a margin cost the limits as they do on hardware
and the 4:3 part is identical. A margin shows the sprites the VDP would put there if the line were wider.
The game writes the whole 16-bit x word and the VDP keeps 9 bits (`xfull`): where the VDP shows such a
sprite inside the 4:3 picture so does the wide view (identical centre), otherwise it is placed where the game
put it, so a sprite at x = 449 (word `$0241`) is not drawn as a phantom at -63, and one at -167 (word `$FFD9`)
not at 345 (the helicopter in level 7, the truck in level 3: 17,000 such pieces in the tests).

## The gun

The Menacer's X comes from the light-gun counter, which the game turns into 2..320 (its H table is capped),
and Y is clamped to 24..200; shots and hit tests use exactly the game's p1_x / p1_y. They cannot reach a margin:
the gun cannot be aimed outside the game's 320 pixels and the hit test knows nothing of what is there.
The choice (`mouse_move_frame`):

- the pointer maps through the wide frame to game x = frame x - margin; over the 320 pixels it is exactly the
  4:3 mapping (and the 1-2 px aim correction, docs/gun.md, applies), so the crosshair lands under the pointer
  and the shot hits what is under it;
- over a margin the crosshair waits at the nearest edge of the game's 320 pixels, and the **trigger and the
  missile button do nothing** (Start still works). A shot from a margin would hit whatever is at the edge,
  not what the player sees under the pointer, which is the one thing the aim must not do; an enemy in a margin
  is shot after it has come in. The crosshair is the game's sprite, so it is drawn at the edge;
- menus and other pillarboxed screens: the pointer's x is irrelevant (rows by y); a click on a black side
  is a click like any other.

`widetest.sh mouse` (script `port/scripts/wide/mouse.txt`): pointer at frame x 200 -> p1_x 162 (the aim is
within 2), x 40 -> 2, x 356 -> 316, (100,150) -> (60,150); over the left and right margin p1_x 2 / 320, shot
and missile counters unchanged after clicks; the same clicks over the playfield fire.

## Tests and results

`port/tools/widetest.sh` (all on a VM, `--hle all`, native runner):

| test | result |
|---|---|
| `widetest.sh mouse` | PASS, 11 checks: frame x 200 -> p1_x 162 (game aim within 2), x 40 -> 2, x 356 -> 316, (100,150) -> (60,150); pointer over the left / right margin: p1_x 2 / 320; trigger and missile over a margin change nothing; the same clicks over the playfield fire (shot resolved, missiles 25 -> 24) |
| `widetest.sh state` | PASS: gun_01 gun_02 gun_04 gun_05 gun_07 title, 3000 frames of the lockstep input each, `--hle all`, native runner: per-frame hash of RAM, VRAM, CRAM, VSRAM, VDP registers and the cycle count identical with `off`, `16:9` and `21:9` (12 comparisons) |
| `widetest.sh stats` (16:9) | 0 centre differences (the 4:3 part of every wide line equals the 4:3 line) over 9 states x 3000 frames: gun_01..gun_08 and title (title: 2532 frames wide, 470 pillarboxed) |
| route runs with the counters | coldboot 9000 frames (4691 wide, 4313 pillarboxed), levelflow 43919 (38398 / 5521), screens 34906 (18300 / 16606), ringstate 53768 (all wide): 0 centre differences in all four |
| all existing gates, widescreen off | wide 92/92 (`--hle all`, 3000 frames), coldboot 9000, ringstate 53768, pause 5738, levelflow 43913, screens 34882 frames identical, lockstep 6 states and `--z80` identical, mousetest ALL PASS, `make` build/t2.md OK, romscan CLEAN, rendergate 92/92 states (276000 frames) and 5/5 routes (147301 frames) identical |
| the same gates, `T2_WIDE=16:9` (hle none vs all, the wide view drawing in both) | wide 92/92, coldboot 9000, ringstate 53768, pause 5738, levelflow 43913, screens 34882 frames identical, lockstep `--z80` 6/6 identical (the two long routes need `LOCKSTEP_TIMEOUT=900`: the 120 s per-run limit of lockstep.py is too short for the 68k run with the extra drawing) |
| browser (`tools/vm/webtest.sh` on t2-vm2) | PASS for the plain page (hle none), `?hle=all` and `?hle=all&wide=16:9` (400-wide canvas; aim points, fire, menu flow by mouse through the margin offset), 0 failures, logic 60.2 fps |

Right-margin staleness (`stale` counter): 0 in gun_01..06, coldboot and screens; gun_07 23 cells in 8 frames,
gun_08 19 in 2, levelflow 326 in 9 frames, ringstate 4859 in 60. The cases looked at (ringstate frame 42, plane A
rows 4, 10, 16: 0 -> text tiles) are the game drawing text-box and effect tiles into the plane in place, not a
streamer that is late; no case of a stale background column was found.
Wrapped x words: 5288 sprite lines in gun_03, 1040 in gun_05, 12384 in gun_07, 19264 in levelflow (the truck,
the helicopter, the van), none anywhere shown wrongly: the centre counter is 0.

`widetest.sh shots` writes `runs/widetest/wide_*.png` (10 states, and the same frame with the pointer at frame
x 320 for the crosshair). Look at `wide_gun_04_aim.png` (the crosshair under the pointer), `wide_title.png`
(pillarbox), `wide_gun_05.png`, `wide_hk_a.png` (the HK boss, both margins).

## Known limits

- **Not combined with high frame rate** (docs/hifps.md): its in-between pictures are drawn from 320-pixel scenes. With both
  requested widescreen wins, the frame rate stays 60 Hz (message on stderr; F4 says so; web: `fps` is ignored).

- Actors appear (are spawned) by the game at its own x; in the 16:9 margin an enemy can pop in 20-40 px inside
  the view. Nothing in the game's logic knows about the margins (no gameplay mode).
- Over a margin the crosshair stops at the edge and the buttons are held back (above).
- Room-level screens (the level summary, menus, logos) are 4:3 with black sides.
- After loading a savestate (F9) the left margin shows the plane as it is until each cell has been seen.
- 21:9 and wider are capped at 480 px.
