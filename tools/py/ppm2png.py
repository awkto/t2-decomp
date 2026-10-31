"""Convert binary PPM (P6) screenshots to PNG, standard library only.
usage: python3 -I tools/py/ppm2png.py [--scale N] IN.ppm [IN2.ppm ...]   (writes IN.png next to each)"""
import struct
import sys
import zlib


def read_ppm(path):
    d = open(path, 'rb').read()
    toks, i = [], 0
    while len(toks) < 4:
        while d[i:i + 1].isspace():
            i += 1
        j = i
        while not d[j:j + 1].isspace():
            j += 1
        toks.append(d[i:j])
        i = j
    assert toks[0] == b'P6' and toks[3] == b'255', path
    return int(toks[1]), int(toks[2]), d[i + 1:]


def write_png(path, w, h, rgb, scale=1):
    rows = []
    for y in range(h):
        row = rgb[y * w * 3:(y + 1) * w * 3]
        if scale > 1:
            row = b''.join(row[x * 3:x * 3 + 3] * scale for x in range(w))
        rows += [b'\0' + row] * scale

    def chunk(t, b):
        c = struct.pack('>I', len(b)) + t + b
        return c + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)

    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w * scale, h * scale, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


if __name__ == '__main__':
    args = sys.argv[1:]
    scale = 1
    if args[:1] == ['--scale']:
        scale = int(args[1])
        args = args[2:]
    for p in args:
        w, h, rgb = read_ppm(p)
        write_png(p.rsplit('.', 1)[0] + '.png', w, h, rgb, scale)
