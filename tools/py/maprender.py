"""Render a level's scrolling background map from the ROM.

usage: python3 -I tools/py/maprender.py LEVEL OUT.png [--plane A|B|both]

Decodes the level background map format (see docs/formats.md): the scroll script
(`scroll_script`) splits the 28 cell rows of each plane into layers, each layer has
a byte map of block numbers, and each block is a tilemap rectangle in ROM.  Tiles
come from the per-level LoadTiles calls, colours from the per-level palette lines.

Standard library only; writes a PNG with zlib.
"""
import struct
import sys
import zlib

ROM = open('rom/baserom.md', 'rb').read()


def u16(a):
    return struct.unpack_from('>H', ROM, a)[0]


def s16(a):
    return struct.unpack_from('>h', ROM, a)[0]


def u32(a):
    return struct.unpack_from('>I', ROM, a)[0]


# ---------------------------------------------------------------------------
# Per-level data, read out of src/t2.s (streamers BgStreamL1/L2/L4, loaders
# sub_002980/00241E/002632, palettes via Copy32).
#   scroll   : scroll_script address used during the scrolling part of the level
#   planeX   : a2 map-pointer table, a4 block-pointer table, a5 tile-base RAM
#              address table, d2 block width in tiles, ormask ($FFA0AA)
#   bases    : RAM word address -> tile base value (set by the level loader)
#   tilesets : (tile base value, ROM address of the LoadTiles length word)
#   palettes : 4 palette line ROM addresses
# ---------------------------------------------------------------------------
LEVELS = {
    1: dict(
        scroll=0x4A94,
        planeA=dict(a2=0x4F08, a4=0x4F48, a5=0x4FDC, d2=8, ormask=0x8000),
        planeB=dict(a2=0x4DA4, a4=0x4F48, a5=0x4FDC, d2=16, ormask=0x0000),
        # loader VdpQueueTileRect fills: (rom tilemap, x, y, w, h, ormask, base ram)
        fillB=[(0x046C44, 0, 2, 32, 24, 0x0000, 0x9E38),
               (0x046C44, 32, 2, 32, 24, 0x0000, 0x9E38)],
        # $FFA236 == 0 at the start of the level: plane A streams nothing
        skipA=True,
        bases={0x9E38: 0x001, 0x9E30: 0x401},
        tilesets=[(0x001, 0x0433E2), (0x401, 0x028406)],
        palettes=[0x84BA, 0x847A, 0x83FA, 0x843A],
    ),
    2: dict(
        scroll=0x4C64,
        planeA=dict(a2=0x522E, a4=0x52C0, a5=0x52EC, d2=8, ormask=0x8000),
        planeB=dict(a2=0x5256, a4=0x52C0, a5=0x52EC, d2=8, ormask=0x0000),
        fillA=[(0x073132, 0, 2, 24, 24, 0x8000, 0x9E38),
               (0x073142, 24, 2, 16, 24, 0x8000, 0x9E38),
               (0x073142, 40, 2, 8, 24, 0x8000, 0x9E38),
               (0x073172, 48, 2, 8, 24, 0x8000, 0x9E38)],
        fillB=[(0x072232, 0, 2, 32, 24, 0x0000, 0x9E38),
               (0x072232, 32, 2, 32, 24, 0x0000, 0x9E38),
               (0x0729B2, 32, 10, 32, 11, 0x0000, 0x9E38)],
        bases={0x9E38: 0x001},
        tilesets=[(0x001, 0x06F4D0), (0x16C, 0x073902), (0x22A, 0x07E2B0)],
        palettes=[0x851A, 0x855A, 0x83FA, 0x843A],
    ),
    4: dict(
        scroll=0x4BB4,
        planeA=dict(a2=0x5026, a4=0x51A4, a5=0x5200, d2=8, ormask=0x8000),
        planeB=dict(a2=0x50C8, a4=0x51A4, a5=0x5200, d2=8, ormask=0x0000),
        fillB=[(0x065264, 0, 2, 32, 24, 0x0000, 0x9E38),
               (0x065264, 32, 2, 32, 24, 0x0000, 0x9E38)],
        bases={0x9E38: 0x001, 0x9E46: 0x1C4},
        tilesets=[(0x001, 0x061C22), (0x1C3, 0x068648), (0x257, 0x0BCBBA),
                  (0x3B2, 0x0473C4), (0x5B7, 0x02585A), (0x670, 0x025DFC)],
        palettes=[0x84BA, 0x849A, 0x83FA, 0x843A],
    ),
}

SCREEN_ROWS = 28          # 224 pixels / 8
COL_WORDS = 0x50 // 2     # block tilemap row stride: $50 bytes = 40 words


def parse_scroll(addr):
    """Return (planeA_groups, planeB_groups); each group = (rows, speed)."""
    a, b, plane = [], [], 'A'
    p = addr
    while True:
        rows = s16(p)
        if rows == -1:
            break
        if rows == 0:
            plane = 'B'
            p += 2
            continue
        speed = u16(p + 2)
        (a if plane == 'A' else b).append((rows, speed))
        p += 4
    return a, b


def decode_map(addr):
    """Map byte array terminated by 0. Returns list of block bytes."""
    out = []
    while ROM[addr]:
        out.append(ROM[addr])
        addr += 1
    return out


def block_rect(ptr, width, height, base, ormask, mirror):
    """Expand one block into `height` rows of `width` tile words."""
    rows = []
    for y in range(height):
        line = []
        for x in range(width):
            if mirror:
                x = width - 1 - x
            w = (u16(ptr + y * 0x50 + x * 2) + base) & 0xFFFF
            if mirror:
                w ^= 0x800
            line.append(w | ormask)
        rows.append(line)
    return rows


def decode_fill(ptr, x0, y0, w, h, ormask, base, width):
    """Expand a loader VdpQueueTileRect into (x, y) -> tile word.

    The fill sits in the wrapping 64-cell plane, so repeat it every 64 columns
    (a static layer keeps showing the same screen of tiles as the plane wraps).
    """
    out = {}
    for y in range(h):
        for x in range(w):
            word = (u16(ptr + y * 0x50 + x * 2) + base) & 0xFFFF
            for k in range(0, width // 64 + 2):
                out[(x0 + x + 64 * k, y0 + y)] = word | ormask
    return out


def decode_plane(plane_cfg, groups, base_of, fills=()):
    """Build a plane as SCREEN_ROWS x W list of tile words (None = empty)."""
    a2, a4, a5, d2, ormask = (plane_cfg['a2'], plane_cfg['a4'], plane_cfg['a5'],
                              plane_cfg['d2'], plane_cfg['ormask'])
    # place layers top to bottom; keep the widest layer
    layers = []   # (row_offset, rows, [ [tileword,...] per column ])
    row_off = 0
    for gi, (rows, speed) in enumerate(groups):
        mapptr = u32(a2 + 4 * gi)
        width = 0
        if speed and mapptr:
            blocks = decode_map(mapptr)
            cols = []
            for blk in blocks:
                if blk == 0x80:
                    cols.append([[None] * d2 for _ in range(rows)])
                    continue
                mirror = blk >= 0x80
                idx = (256 - blk) if mirror else blk
                bptr = u32(a4 + 4 * idx)
                ramaddr = u16(a5 + 2 * idx)
                base = base_of.get(ramaddr, 0)
                cols.append(block_rect(bptr, d2, rows, base, ormask, mirror))
            width = len(cols) * d2
            layers.append((row_off, rows, cols))
        row_off += rows

    width = max([w for (_, _, cols) in layers for w in [len(cols) * d2]] or [1])
    # static loader fills (before streaming layers overwrite them)
    fill = {}
    for (ptr, x0, y0, w, h, fmask, bram) in fills:
        fill.update(decode_fill(ptr, x0, y0, w, h, fmask, base_of.get(bram, 0), width))
    img = [[fill.get((x, y)) for x in range(width)] for y in range(SCREEN_ROWS)]
    for (roff, rows, cols) in layers:
        for ci, col in enumerate(cols):
            for y in range(rows):
                for x in range(d2):
                    img[roff + y][ci * d2 + x] = col[y][x]
    return img


def tile_atlas(tilesets):
    """Return f(tile_index) -> 32 ROM bytes, from the LoadTiles tables."""
    sets = []
    for base, addr in tilesets:
        length = u16(addr)
        sets.append((base, addr + 2, length // 32))
    sets.sort()

    def fetch(idx):
        base, data, count = None, None, None
        for b, d, n in sets:
            if idx >= b:
                base, data, count = b, d, n
        if base is None or idx - base >= count:
            return None
        o = data + (idx - base) * 32
        return ROM[o:o + 32]
    return fetch


def tile_pixels(b):
    """Decode 32 bytes into 8x8 palette indices (packed 4bpp: 2 px/byte)."""
    px = []
    for y in range(8):
        row = []
        for byte in b[y * 4:y * 4 + 4]:
            row.append(byte >> 4)
            row.append(byte & 0xF)
        px.append(row)
    return px


def palette_lines(palettes):
    lines = []
    for addr in palettes:
        lines.append([u16(addr + 2 * i) for i in range(16)])
    return lines


def color8(word):
    """Genesis CRAM word 0000BBB0GGG0RRR0 -> 8-bit RGB, matching the core."""
    def ch(c):
        v = c * 2                      # 3-bit -> 4-bit
        v5 = (v << 1) | (v >> 3)       # 4-bit -> 5-bit, as the core does
        return v5 << 3
    return (ch((word >> 1) & 7), ch((word >> 5) & 7), ch((word >> 9) & 7))


def render(level, which):
    if level not in LEVELS:
        raise SystemExit('maprender: level %d has no background streamer (have %s)'
                         % (level, sorted(LEVELS)))
    cfg = LEVELS[level]
    a_groups, b_groups = parse_scroll(cfg['scroll'])
    base_of = cfg['bases']
    fetch = tile_atlas(cfg['tilesets'])
    pals = palette_lines(cfg['palettes'])

    # back to front: plane B then plane A
    order = {'B': 'B', 'A': 'A', 'both': 'BA'}
    names = order[which]
    planes = {}
    for name in names:
        cfgp = cfg['plane' + name]
        groups = [] if (name == 'A' and cfg.get('skipA')) else \
            (a_groups if name == 'A' else b_groups)
        planes[name] = decode_plane(cfgp, groups, base_of, cfg.get('fill' + name, ()))

    w = max(len(p[0]) for p in planes.values())
    H, W = SCREEN_ROWS * 8, w * 8
    rgb = [[(0, 0, 0)] * W for _ in range(H)]
    for y in range(SCREEN_ROWS):
        for x in range(w):
            for name in names:          # later planes draw in front
                img = planes[name]
                if x >= len(img[0]):
                    continue
                wrd = img[y][x]
                if wrd is None:
                    continue
                idx = wrd & 0x3FF
                pal = (wrd >> 13) & 3
                hf = (wrd >> 11) & 1
                vf = (wrd >> 12) & 1
                b = fetch(idx)
                if b is None:
                    continue
                px = tile_pixels(b)
                for py in range(8):
                    sy = 7 - py if vf else py
                    for pxi in range(8):
                        sx = 7 - pxi if hf else pxi
                        c = px[sy][sx]
                        if c == 0:
                            continue
                        rgb[y * 8 + py][x * 8 + pxi] = color8(pals[pal][c])
    return rgb


def write_png(path, rgb, scale=1):
    H, W = len(rgb), len(rgb[0])
    rows = []
    for y in range(H):
        row = bytearray()
        for x in range(W):
            row += bytes(rgb[y][x]) * scale
        for _ in range(scale):
            rows.append(b'\0' + bytes(row))

    def chunk(t, b):
        c = struct.pack('>I', len(b)) + t + b
        return c + struct.pack('>I', zlib.crc32(t + c) & 0xFFFFFFFF)

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', W * scale, H * scale, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(b''.join(rows), 6))
    png += chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def main():
    args = sys.argv[1:]
    which = 'both'
    if '--plane' in args:
        i = args.index('--plane')
        which = args[i + 1]
        del args[i:i + 2]
    level, out = int(args[0]), args[1]
    rgb = render(level, which)
    write_png(out, rgb, scale=1)
    print('%s: %dx%d' % (out, len(rgb[0]), len(rgb)))


if __name__ == '__main__':
    main()
