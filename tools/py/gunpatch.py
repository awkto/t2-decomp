#!/usr/bin/env python3
"""Optional ROM patch: remove the Menacer position smoothing (docs/gun.md, "Latency").

    gunpatch.py IN.md OUT.md

MenacerReadXY ($01E442) averages the last 8 or 16 per-frame HV latches, so the crosshair (and the
point that gets shot) trails a moving gun by 4-8 frames. With a mouse there is no light-gun noise to
filter. The patch replaces the averaging body at $01E460 with code that converts only the newest
latch (same H->X table, same Y offset), giving a 1-frame response. Hand-written replacement code
only; no game bytes are carried by this script. The game has no checksum lock (checked: a patched
ROM boots and plays).
"""
import hashlib
import sys

SHA1 = "85cc1cf3379d3ce23ca3c03d84fe6e2b3adc9c56"
ADDR = 0x1E460
END = 0x1E502          # original body ends with rts at $01E500; $01E502 is the "no latch" rts
PATCH = bytes.fromhex(
    "2079ffffa09e"     # movea.l ($FFA09E).l,a0      ring write pointer (next slot)
    "b1fcffffa07e"     # cmpa.l  #$FFFFA07E,a0       at the ring start?
    "6606"             # bne.s   +6
    "41f9ffffa09e"     # lea     ($FFA09E).l,a0      wrap to the ring end
    "7000"             # moveq   #0,d0
    "1020"             # move.b  -(a0),d0            newest H counter
    "43f9ffff9f7e"     # lea     ($FF9F7E).l,a1      H -> X/2 table
    "10310000"         # move.b  (a1,d0.w),d0
    "d040"             # add.w   d0,d0               X
    "33c0ffff9f6e"     # move.w  d0,($FF9F6E).l
    "7200"             # moveq   #0,d1
    "1220"             # move.b  -(a0),d1            newest V counter
    "d279ffff9f78"     # add.w   ($FF9F78).l,d1      + Y offset (8)
    "33c1ffff9f70"     # move.w  d1,($FF9F70).l
    "4e75"             # rts
)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    d = bytearray(open(sys.argv[1], "rb").read())
    if hashlib.sha1(d).hexdigest() != SHA1:
        print("unexpected ROM (want raw .md, sha1 %s)" % SHA1)
        return 1
    assert len(PATCH) <= END - ADDR
    d[ADDR:ADDR + len(PATCH)] = PATCH
    open(sys.argv[2], "wb").write(d)
    print("patched %d bytes at $%06X -> %s" % (len(PATCH), ADDR, sys.argv[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
