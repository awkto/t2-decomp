# VDP: layout, the command list, scrolling, palettes (GitLab #7)

Names are in `data/symbols.txt` / `data/ram.txt` ("# --- engine (issue #7)"). Sprites are in
`sprite.md`, frame order in `mainloop.md`.

## Layout (gameplay)

Register dump of level 1 (harness `vdp`, 600 frames after the `l1` state), matching `VdpRegTable`
`$00869C` and the constants in the code:

| reg | value | meaning |
| --- | --- | --- |
| 0 | `$04` | HInt off, HV latch off (the Menacer still latches, GPGX forces it; `docs/gun.md`) |
| 1 | `$74` | display on, VInt on, DMA on, V28 |
| 2 | `$38` | plane A at `$E000` |
| 3 | `$38` | window at `$E000` (window not used: reg 17/18 = 0) |
| 4 | `$06` | plane B at `$C000` |
| 5 | `$7A` | sprite table at `$F400` |
| 7 | `$00` | backdrop colour 0 |
| 11 | `$0A` | ExtInt on (gun), hscroll **per 8-line cell row**, vscroll full screen |
| 12 | `$81` | H40 (320 px), no shadow/highlight |
| 13 | `$3C` | hscroll table at `$F000` |
| 15 | `$02` | autoincrement 2 (the list commands change it temporarily) |
| 16 | `$01` | planes 64 x 32 cells |

Tiles below `$C000`. The game keeps a RAM shadow of regs 0/1 in `vdp_reg01` `$FFA0DA` (`VdpSetRegs`)
and sets/clears the DMA enable bit (reg 1 bit 4) around every DMA.

## The VDP command list

Nothing writes the VDP from an interrupt. During the frame, code appends commands to a list in RAM;
after the VBlank wait `VdpListRun` `$011D32` executes them (`mainloop.md`). This is the only
"DMA queue" of the engine, and despite the name most entries are CPU copies.

- `vdp_list` `$FFB252`, write pointer `vdp_list_ptr` `$FF80BC`; command data that is generated on
  the fly (tilemap rectangles) goes to `vdp_list_area` `$FFB452`, pointer `vdp_list_data` `$FF80B8`.
- `VdpListReset` `$011D12` rewinds both pointers (start of the frame), `VdpListEnd` `$011D28` writes
  command 0, `VdpListRun` interprets: `move.w (a6)+,d0; jmp VdpListJump(pc, d0*4)`.

| cmd | handler | args (words unless noted) | effect | queued by |
| --- | --- | --- | --- | --- |
| 0 | `$011D64` | | end; rewinds the list | `VdpListEnd` |
| 1 | `$012068` | count, src.l, VRAM addr | CPU copy of `count` words, autoinc 2 | `BuildSpriteTable` (SAT → `$F400`), script `load_tiles`, `$009B00` |
| 2 | `$011D70` | bytes, VRAM addr | fill with 0 (`VramFillWords`) | `VdpQueueFillRows` `$012212`, text clear, empty map columns |
| 3 | `$011D90` | value, VRAM addr | one word (`VramWriteWord`) | |
| 4 | `$011D9E` | CRAM addr, src.l | 16 words to CRAM (one palette line) | |
| 5 | `$011D80` | bytes, VRAM addr, value | fill with a value | |
| 6 | `$0120BA` | cols, src.l, VRAM addr, rows-1 | tilemap rectangle, rows `$80` bytes apart (64-cell planes) | `VdpQueueTileRect` `$012234` (background columns, HUD boxes) |
| 7 | `$011E48` | (unused), src.l, VRAM addr | hscroll: 28 words with source stride `$20` and autoinc `$20` for plane A, then the same at +2 for plane B | `ScrollRows`, `HScrollClear` |
| 8 | `$011F6E` | vscroll A, vscroll B | VSRAM 0/1 | `VdpQueueVScroll` `$011F58` |

Measured per frame on level 1 (`watch C00000`): one hscroll upload (56 words), one SAT copy
(sprite count x 4 words) and a few single words; background columns come in bursts when a layer
crosses a column.

Immediate (not queued) helpers, used by loaders and blocking screens: `DmaToVram` `$011FA2` (args in
`dma_args` `$FFA0E0`: length, source, VRAM address; `LoadTiles` `$01320A` = "a0 = length word +
tiles, d1 = tile index"), `VramFillDma` `$011EC4`, `VramClearDma` `$00800C`, `VramClear` `$00871A`,
`VramFillWords` `$008782`, `VramWriteWord` `$0087C4`, `VramWriteLong` `$0087E6`, `CramClear`
`$008162`, `VsramClear` `$00818C`, `CramUpload` `$0080FE`. The HUD also writes VRAM directly during
the frame.

## Horizontal scrolling and the background

The stages scroll sideways by **cell-row parallax**, not by moving the camera: every 8-line row of
both planes has its own hscroll value (reg 11 per-cell mode), and the objects ride those rows.

### Rows (`ScrollRows` `$004752`, every frame)

`scroll_script` `$FF9E98` points to a ROM list of groups:

    rows.w speed.w     ; next `rows` cell rows move at `speed` (8.8 px/frame)
    0                  ; switch from plane A to plane B (column +2 of the hscroll entries)
    -1                 ; end

For each group an 8.8 accumulator in `scroll_acc` `$FFAF92` advances by `speed`; the whole-pixel
step d is subtracted from the hscroll entry of each row in the group (`hscroll_ram` `$FFAA5C`, one
entry per row, `$20` apart, A word then B word), added to `row_pos` `$FFAFD2`, and stored in
`row_delta` `$FFB0D2`. Then command 7 uploads the table to `$F000`. Rows are numbered plane A first,
then plane B, so `row_delta[+6 + 28]` (`ObjScrollX`) gives an object the step of its row.

The script switches the table (op `$05` → `$004A88`, `$06` → `$004A94`, or `poke_long` to
`scroll_script`); speed 0 groups stand still (sky, HUD rows).

### Map columns (`BgStreamLayers` `$00498A`)

Called by the per-level routines `BgStreamL1` `$004916`, `BgStreamL2` `$004872`, `BgStreamL4`
`$0048C4`, `BgStreamL5` `$004820`, `BgStreamL6` `$0047CE`. `GameLoop` calls none for levels 3,
7 and 8: there the planes only scroll by hscroll (wrapping around the 64-cell plane) and the level
ends through the script (level 3 has an `end_level` command) or other code (not traced). Each sets a6 = per-layer "next column" positions, a1 = the
layers' `row_pos`, a2 = list of per-layer map byte arrays, a4/a5 = per-level tables of block
graphics pointers and tile-base words, d0 = plane address (`$E000` A or `$C000` B), d1 = column step,
d2 = rows. Per layer:

1. when the layer's row position reaches the next trigger, the trigger advances and the next map
   byte is read (index = position / 64 or / 128),
2. byte > 0: block number → tilemap rectangle (`VdpQueueTileRect`, cmd 6) into the plane column that
   just scrolled off (`(pos - step)/4 & $7F` + plane address, 64-cell wrap),
3. byte `$80`: empty column (`VdpQueueFillRows`, cmd 2); other negative bytes: alternative blocks
   (`$01227A`),
4. byte 0: end of the map, `level_exit = $FF`. **The length of a level's scrolling map is the
   length of the level.**

## Palettes

- `pal_cur` `$FFA1B4` (64 words) is what is in CRAM, `pal_target` `$FFA134` what a fade moves to.
  Level loaders and `ScrLoadPalette` (`$00A2A8`) write `pal_target` lines with `Copy32` `$008A7C`.
- `PalFadeStep` `$008970`: every colour of `pal_cur` steps one level per channel (R, G, B
  independently, 2 per CRAM step) towards `pal_target`; changed entries are flagged in `pal_changed`
  `$FFA0F4`; when nothing changed it clears `pal_fading` `$FFA246`.
- `PalFade` `$008A4C` (blocking): `WaitVBlank`, `PalFadeStep`, `WaitVBlank`, short delay,
  `CramUpload` of all 64 colours, until `pal_fading` is clear. At most 7 steps = about 14 frames.
  Fade out = `PalSetTargetBlack` `$008A18` then `PalFade`; fade in = clear `pal_cur`
  (`PalClearCurrent` `$008A06`), set `pal_target`, `PalFade`.
- `CramUploadChanged` `$00812A` uploads only the flagged entries.
- `PalCycle` `$00E9FA` (when `pal_cycle_on` `$FFA0A4`): every 8 frames copies 8 bytes (4 colours) from
  the 4-step table `$0084FA` into colours 12..15 of palette line 0 and uploads CRAM.
- Gameplay palette changes are immediate: list command 4 or `CramUpload`.
