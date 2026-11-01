# Level background map format (issue #maps)

How a level's scrolling background is stored in ROM and streamed to the VDP.
Renderer: `tools/py/maprender.py` (standard library only):

    .venv/bin/python -I tools/py/maprender.py 1 runs/level1.png        # all planes
    .venv/bin/python -I tools/py/maprender.py 4 runs/l4B.png --plane B # one plane

The engine side (parallax, VDP command list) is `docs/subsystems/vdp.md`; the script
interpreter that selects the tables is `docs/subsystems/levels.md`.

## The three data sets of a level

1. a **script** (enemies, texts, palettes, `end_level`);
2. a **scroll script** (`scroll_script` `$FF9E98`): cell-row speeds, read by
   `ScrollRows` `$004752`, and at the same time the layer list read by `BgStreamLayers`
   `$00498A`;
3. the **background maps**: per layer a byte array of block numbers, plus the block
   tilemaps, the tile-base table and the tile graphics.

Only the first is per-level code; the other two are the tables below.

## Scroll script

`scroll_script` (`$FF9E98`) is a ROM word list set by script ops `$05`/`$06` or
`poke_long` (`$18`). A group is `rows.w speed.w`:

* `rows` = number of 8-line cell rows in this layer (1..); the rows of a plane add up
  to 28 (224 lines);
* `speed` = scroll speed in 8.8 fixed point px/frame (0 = the layer is static);
* a single word `0` switches from plane A to plane B;
* a single word `-1` ends the script.

`ScrollRows` advances one 8.8 accumulator per group and writes the whole-pixel step into
the per-row hscroll table (`hscroll_ram` `$FFAA5C`, plane A words then plane B words) and
into `row_pos` `$FFAFD2` / `row_delta` `$FFB0D2`. `BgStreamLayers` walks the same list:
the plane A groups are those before the `0`, the plane B groups those after, up to `-1`.

Known scroll scripts: `ScrollTab_4A88` `$004A88` (all static, intro), `ScrollTab_4A94`
`$004A94` (level 1), `ScrollTab_4C64` `$004C64` (level 2), `ScrollTab_4BB4` `$004BB4`
(level 4), `$004C90`/`$004CB8` (levels 3/5 region), `$00460C` (story intro).

## Streamer tables (per level)

`BgStreamL1/L2/L4/L5/L6` select, per plane, four ROM tables and a plane base:

| register | meaning |
| --- | --- |
| `a2` | map-pointer table: one long per scroll-script group, in order (0 = no map) |
| `a4` | block-pointer table: map byte -> long ROM address of the block tilemap |
| `a5` | tile-base table: map byte -> word RAM address holding that block's tile base |
| `d0` | plane VRAM base (`$E000` A, `$C000` B) |
| `d1` | column width in pixels (64 for 8-tile blocks, 128 for 16-tile blocks) |
| `d2` | block width in tiles (8 or 16) |
| `$FFA0AA` | OR mask applied to every tile word (priority/palette bits) |

| level | maps A | maps B | blocks | bases | d2 A / B | ormask A / B |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | `BgMapsL1A` `$004F08` (`$FFA236 == -1`), `BgMapsL1Aalt` `$004EE8` (`== 1`) | `BgMapsL1B` `$004DA4` | `BgBlocksL1` `$004F48` | `BgBasesL1` `$004FDC` | 8 / 16 | `$8000` / 0 |
| 2 | `BgMapsL2A` `$00522E` | `BgMapsL2B` `$005256` | `BgBlocksL2` `$0052C0` | `BgBasesL2` `$0052EC` | 8 / 8 | `$8000` / 0 |
| 4 | `BgMapsL4A` `$005026` | `BgMapsL4B` `$0050C8` | `BgBlocksL4` `$0051A4` | `BgBasesL4` `$005200` | 8 / 8 | `$8000` / 0 |
| 5 | `BgMapsL5A` `$005302` | `BgMapsL5B` `$0053A6` | `BgBlocksL5` `$0053FE` | `BgBasesL5` `$00542A` | 8 / 16 | `$E000` / 0 |
| 6 | `BgMapsL6A` `$005440` | `BgMapsL6B` `$00548A` | `BgBlocksL6` `$0054D0` | `BgBasesL6` `$00553C` | 8 / 8 | `$8000` / 0 |

Level 1 has a phase variable `$FFA236` (script `poke_word`, cleared at level load):
`0` = plane A is skipped entirely, plane B streams (this is the level start, the screenshot
in `docs/levels.md`); `-1` and `1` stream both planes with different plane A map tables.
Levels 3, 7, 8 have no streamer (their planes only scroll by hscroll).

## Map byte array

`BgStreamLayers` reads one byte per column from the layer's map array (from the `a2`
pointer) as the layer's `row_pos` passes the next trigger:

* `1..$7F` = block number: draw the block (below) at the column that just scrolled off;
* `$80` = empty column (`VdpQueueFillRows`, command 2);
* `$81..$FF` = mirrored block `256 - byte`: same tilemap but drawn reversed and with the
  h-flip bit toggled (`VdpQueueTileRectMirror` `$01227A`);
* `0` = end of the map; sets `level_exit` `$FFA234` = `$FF` (**the map length is the
  length of the level**).

For 16-tile blocks (level 1 plane B, level 5 plane B) the byte index is
`row_pos / 128 + 3`; for 8-tile blocks it is `row_pos / 64` (the `+3` is why the first
three bytes of those arrays are a header the streamer never reads). The destination column
is `((row_pos - step) / 4) & $7F` added to the plane base (bytes; 64-cell wrap).

## Block

A block is a tilemap rectangle `d2` tiles wide and `rows` tiles tall (`rows` = the group's
row count). It is stored in ROM as tile words with a fixed row stride of `$50` bytes
(40 words), read by `VdpQueueTileRect` `$012234`:

    out_word = (block_word + tile_base) | $FFA0AA

`tile_base` is the value of the RAM word named by the `a5` entry (set by the level loader,
e.g. `$FF9E38` = 1 for the level's main tileset). The resulting word is a standard VDP
tile word: bits 0-10 tile index, 11 h-flip, 12 v-flip, 13-14 palette, 15 priority.

## Tiles

Tile graphics are uncompressed, loaded by `LoadTiles` `$01320A` (`a0` = `len.w` then
`len/2` words, `d1` = first VRAM tile index). A tile is 32 bytes. **The bytes are packed
4 bpp, two pixels per byte (high nibble first)**, the standard Mega Drive
mode-5 tile format (row-major, 4 bytes per row).

Tile bases and graphics per level (from the loader and the script's `load_tiles` ops):

| level | base | graphics |
| --- | --- | --- |
| 1 | `$001` | `$0433E2` (`LevelLoad1`); `$401` | `$028406` (script t=8758) |
| 2 | `$001` | `$06F4D0` (`LevelLoad2`); `$16C` | `$073902`; `$22A` | `$07E2B0` |
| 4 | `$001` | `$061C22` (`LevelLoad4`); `$1C4` | `$068648` (loaded at `$1C3`) |

A tile index is resolved to the tileset with the largest base <= index.

## Palettes

The level loader writes the four palette lines with `Copy32` `$008A7C` into
`pal_target` `$FFA134`; `PalFade` uploads them to CRAM. Words are the hardware
`0000BBB0GGG0RRR0` format.

| level | line 0 | line 1 | line 2 | line 3 |
| --- | --- | --- | --- | --- |
| 1 | `$0084BA` | `$00847A` | `$0083FA` | `$00843A` |
| 2 | `$00851A` | `$00855A` | `$0083FA` | `$00843A` |
| 4 | `$0084BA` | `$00849A` | `$0083FA` | `$00843A` |

## Static plane fills

The loader also pre-fills both planes with `VdpQueueTileRect` rectangles (the sky gradient
etc.) that the streamed columns later overwrite. `maprender.py` draws them first:
level 1 `dat_046C44` 32x24 at plane B `$C100`/`$C140`; level 2 `dat_072232` 32x24 at
`$C100`/`$C140`, `dat_0729B2` 32x11 at `$C540`, and plane A `dat_073132`/`dat_073142`/
`dat_073172` at `$E100`/`$E130`/`$E150`/`$E160`; level 4 `dat_065264` 32x24 at
`$C100`/`$C140`.

## Verification

`maprender.py` output was compared with headless harness screenshots (GPGX,
`gun_01/02/04` states + ~1200-1500 frames, `tools/harness/harness`): levels 1, 2 and 4
match in tiles, colours and layout (sky/skyline/ground for 1, hideout interior for 2,
red sky/fence/ground for 4). Level 1 is rendered with `$FFA236 == 0` (plane A skipped),
the phase the level starts in.

## Open / uncertain

* The exact initial `row_pos` / trigger values (which decide the very first streamed
  column) were not traced; the render lays columns out from map byte 0, so it is the whole
  level strip, not a particular camera offset.
* Levels 3, 7, 8 have no background streamer (no maps). `maprender.py` is configured for
  levels 1, 2 and 4; levels 5 and 6 use the same tables (`BgMapsL5A/B`, `BgBlocksL5`,
  `BgBasesL5`, `BgMapsL6A/B`, `BgBlocksL6`, `BgBasesL6`) but their tile loads/palettes
  were not added, so they are not rendered yet.
* The `+3` header bytes of the 16-tile map arrays are read past, not understood.
