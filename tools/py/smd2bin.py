"""De-interleave an SMD dump (512-byte header, 16 KiB blocks: odd bytes then even bytes) into a raw ROM.
usage: python3 -I tools/py/smd2bin.py IN.smd rom/baserom.md   (expects sha1 85cc1cf3379d3ce23ca3c03d84fe6e2b3adc9c56)"""
import hashlib, sys
d = open(sys.argv[1], 'rb').read()
if len(d) % 16384 == 512:
    d = d[512:]
    out = bytearray()
    for b in range(0, len(d), 16384):
        blk = d[b:b + 16384]; o = bytearray(len(blk))
        o[1::2] = blk[:len(blk) // 2]; o[0::2] = blk[len(blk) // 2:]
        out += o
    d = bytes(out)
open(sys.argv[2], 'wb').write(d)
print(hashlib.sha1(d).hexdigest(), sys.argv[2])
