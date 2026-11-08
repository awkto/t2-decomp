# Native renderer (GitLab github/t2-decomp#12; prerequisite for widescreen #13 and high fps #14)

Code: `port/src/render/` (`render.c/.h` the renderer, `render_emu.c/.h` the emulator bridge and the
compare mode), `tools/patches/gpgx-render.patch` (three timing hooks in GPGX's line renderer),
`gx_set_render_hooks` / `gx_sat` / `gx_viewport_*` in `port/hle/gx.c`. Gate: `port/tools/rendergate.sh`.
The design and the pixel pipeline are those of the sibling project scooby-decomp
(`docs/subsystems/render.md`), without its verb-bar band and widescreen view, which T2 does not need.

## Selecting the renderer

```
build/port/t2port --render c           # or T2_RENDER=c, or  render = c  in t2.ini
build/port/t2port --render compare     # draw both every frame, show GPGX's, report differences
build/port/t2port --render gpgx        # default: GPGX's picture
```

`T2_RENDER` overrides the option. The picture on screen, `--screenshot`, script `shot`, `emu_video()` and
the web build all see the chosen renderer's frame. GPGX still renders every frame: its status bits
(sprite overflow / collision), its VDP timing and the lockstep hashes are part of the emulated machine,
and the compare mode needs its picture. The extra cost of the C renderer is about 0.1 ms per frame.

Compare mode prints, per differing frame, `rendercmp: frame N: K pixels differ, first at x= line= c= gpgx=`
(RGB565 values; the first 20 only) and a summary at exit:

```
rendercmp: 3000 frames compared, 0 differ, 0 pixels differ (all identical)
```

`T2_RENDER_DUMP=dir` writes the first six differing frames as PPM (`f<frame>-c.ppm` / `-gpgx.ppm`).
A frame in which the C renderer did not draw every line at GPGX's size counts as differing.

## Architecture

```
  68k / C game code --writes--> GPGX VDP state (VRAM, CRAM, VSRAM, regs, sprite cache)
                                   |
        GPGX render_line(l)  --hook-->  render_line_pixels(live view, l)   planes, window, sprites
        GPGX parse_satb(l)   --hook-->  render_parse_sprites(live view, l) sprites for line l+1
        GPGX remap_line(l)   --hook-->  render_line_colors(live view)      indices -> RGBA, line l
                                   |
              g_fb (RGBA, 320 x 224)  --render_emu_frame_end-->  RGB565 frame (emu_video)
```

- `render.c` knows nothing about the emulator. It reads a `render_vdp` view (VRAM as host-endian
  words, CRAM in the VDP's 9-bit form, VSRAM, the 24 registers, the VDP's internal sprite cache and the
  active width) and is driven line by line in the order the VDP works: draw line `l`, parse the sprite
  table for line `l+1`, convert line `l` to colours. The state that persists across lines
  (`render_ctx`: the two per-line sprite lists, the sprite-masking flag) is the VDP's own.
- **Raster mode** (what runs now, `render_emu.c`): the view points at GPGX's live memories and the three
  steps run at the exact moments GPGX renders, so every mid-frame change is seen on the same line as in
  GPGX: a DMA or CRAM write during the display, a scroll or register write on a line. T2's HBlank/raster
  tricks therefore need no special model. This is why the output can be, and is, pixel-identical.
- **Frame mode** (`render_frame`): one call per frame from a `render_scene` (a VDP snapshot plus the
  active height). It exists for what follows: a scene is data, so widescreen and interpolation can build
  their own.

## Pixel pipeline (render.c)

Per line, as the VDP does it (and GPGX bit for bit):
- planes B and A in 2-cell columns from the h-scroll entry of the line (full / cell / line modes),
  v-scroll full-screen or per 2-cell column (the partly shown left column uses the AND of the last two
  VSRAM words in H40), name table sizes from register 16;
- the window: the whole line when the line is on the window's side of register 18, otherwise the
  horizontal split of register 17, including the VDP's "window bug" (the scrolled-in column of plane A
  next to a left window is fetched one column further right);
- sprites from the per-line list (16 / 20 per line, 256 / 320 pixels per line including off-screen
  ones, the link list, at most 64 / 80 entries walked), front to back; sprite masking by x = 0 after a
  sprite with x > 0 (or after a full previous line);
- merge: pixels are `IPpp cccc` bytes; lookup tables built from the priority rules, including
  shadow/highlight (register 12 bit 3);
- left column blanking (register 0 bit 5), display off (register 1 bit 6: backdrop);
- colours: 9-bit CRAM -> 4-bit channels (shadow c, normal 2c, highlight c+7; register 0 bit 2 clear keeps
  one bit per channel) -> 8 bits as c4 * 17. Comparisons convert to GPGX's RGB565 exactly.

Not implemented because the game never uses them: interlace, modes 4 and below, 128 KiB VRAM, GPGX's
"enhanced vscroll" option.

## Next steps (#13 widescreen, #14 high fps) - designed for, not implemented

- **Wider viewport**: `render_vdp.width` is a parameter of every stage and the line buffers have margins
  (`RENDER_MAX_W` is 320 now; raise it with the wider view). A widescreen scene adds a viewport rectangle in
  plane coordinates (output pixel 0 = screen x `x0`, shift the h-scroll by `-x0`, draw `width >> 4`
  columns); the planes wrap in 64-cell name tables, sprites need their x range opened (the 0x80 offset and
  the 256/320-pixel per-line limits become scene parameters), and the game's streaming has to keep more
  columns in VRAM. scooby-decomp's `render_view` / `widescreen.c` is the model.
- **Interpolated positions**: give the scene fractional scroll values (per band) and a sprite list built
  by the game layer from the objects' sub-pixel positions instead of the parsed SAT.
  `render_parse_sprites` is the only reader of the SAT, so a scene can carry its own list. Rendering at N
  times the game rate then means rendering N scenes between two game frames.
- Mid-frame changes the game makes on purpose (if any shows up) can be carried as per-line bands in the
  scene; raster mode needs nothing.

## Verification

```
port/tools/rendergate.sh                                # all states/*.state x 3000 frames + 5 routes
port/tools/rendergate.sh --states "gun_01 title" --routes none --frames 600
T2_RENDER=compare build/port/t2port --script s.txt --outdir out --mute --no-vsync
```

`rendergate` runs `t2port` in compare mode (`--hle all` by default, 4 runs in parallel) over every savestate
with the lockstep input (`lockstep.write_script`) and over `pause_gate`, `levelflow_gate`, `screens_gate`,
`coldboot_gate` and `ringstate_gate`, and requires every frame to be identical to GPGX's. Results are in
the "Result" section below.

## Result

`port/tools/rendergate.sh` (`--hle all`, compare mode, nothing differs):

```
rendergate: states 92/92 identical (276000 frames), routes 5/5 identical (147301 frames)
rendergate: PASS
```

Routes: pause_gate 5738, levelflow_gate 43913, screens_gate 34882, coldboot_gate 9000, ringstate_gate 53768
frames, all pixel-identical to GPGX (same counts as the lockstep route gates). With the native runner
(`T2_NATIVE=1`) the sample states gun_01, title, gun_05 (1500 frames) and pause_gate are identical too.
No frame or feature is known to differ: the game uses H40, shadow/highlight-free planes, the window and
sprites, which are all covered; interlace and the 128 KiB VRAM mode never occur.

A run is only as good as its coverage: a VDP feature the 92 states and 5 routes never exercise (listed
under "Not implemented" above) would show up in compare mode as a differing frame, not silently.
