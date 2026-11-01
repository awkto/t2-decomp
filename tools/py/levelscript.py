#!/usr/bin/env python3
"""Dump the level scripts (the wave/event scripts run by ScriptTick $009358).

    python3 -I tools/py/levelscript.py [LEVEL...]        listing of every level (or the given ones)
    python3 -I tools/py/levelscript.py --stats           opcode use counts per level

Format (docs/subsystems/levels.md): a script is a list of commands
    delay.w  opcode.w  args...
`delay` frames after the previous command (0 = same frame) the handler ScriptOps[opcode]
(long table at $00938A) runs with a0 pointing at its args and must leave a0 after them.
The script of level N starts at the long LevelScripts[N] ($00B36C).

Reads the ROM (rom/baserom.md); prints addresses and decoded arguments only.
"""
import os
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ROM = os.path.join(ROOT, "rom", "baserom.md")
OPTAB = 0x00938A
LEVELTAB = 0x00B36C

# Names: spawn_tNN = spawns an object of type $NN (obj +0 low byte); a/b/c = variants with other
# AI/hit routines.  load_tiles: mode.w (0 whole block, 1/-1 halves, -2..-5 quarters), pointer.l to the
# RAM word holding the destination tile index, graphics.l (len.w + tiles) -> VDP list copy.
# opcode: (name, argument layout).  Layout letters: w = word, l = long, x = word X, y = word Y.
# Special: "goto" "gosub" "return" "native" "text" "end" handled in walk(); "w?" = optional word (see op $12).
OPS = {
    0x01: ("spawn_t01a", "xy"),       # $009486 type $01, hit $0059AC (head box = instant kill)
    0x02: ("spawn_t01b", "xw"),              # $0094F2 type $01, hit $005A48; y: 0 = AI $00F83E with +$28 = 160, <0 = -y + flag b12
    0x03: ("goto", "l"),
    0x04: ("nop", ""),
    0x05: ("scroll_tab_4a88", ""),        # scroll_script = $004A88
    0x06: ("scroll_tab_4a94", ""),        # scroll_script = $004A94
    0x07: ("spawn_t07a", ""),
    0x08: ("spawn_t07b", ""),
    0x09: ("spawn_t09", "ww"),
    0x0A: ("spawn_t0aa", ""),
    0x0B: ("spawn_t0ab", ""),
    0x0C: ("spawn_t0c", ""),
    0x0D: ("spawn_t0d", ""),
    0x0E: ("spawn_t0e", "w"),
    0x0F: ("queue_gfx_0f", ""),
    0x10: ("queue_gfx_10", ""),
    0x11: ("queue_gfx_11", ""),
    0x12: ("spawn_t0fa", "x?"),             # x, w; if w == 0 one more word
    0x13: ("spawn_t0fb", "xw"),
    0x14: ("native", "l"),                 # jmp to a routine (it may read more args)
    0x15: ("spawn_t03", "w"),              # $009646 type $03 at x, y = $150, random variant (hit $005AF8, ai $00D978 hp 15); $FF9E06++
    0x16: ("text", "l"),                   # print a text record (PrintText $007DEE)
    0x17: ("draw_17", ""),
    0x18: ("poke_long", "ll"),             # value.l, address.l
    0x19: ("spawn_t1b", ""),
    0x1A: ("draw_tiles_e51e", ""),         # $00A228: 16x11 tile rect from ROM $0539EE -> plane-A $E51E (OR $E000), tile base $FF9E4A
    0x1B: ("spawn_t17", ""),
    0x1C: ("draw_tiles_e39a", ""),         # $00A10A: 13x13 tile rect from ROM $04E284 -> plane-A $E39A (OR $8000), tile base $FF9E48
    0x1D: ("spawn_t01c", "xy"),
    0x1E: ("spawn_t11", "xy"),
    0x1F: ("spawn_t01d", "xy"),            # $009718 type $01 at x, y (ai $00C564 hit $005BEC); $FF9E06++
    0x20: ("spawn_t1a", ""),
    0x21: ("poke_word", "wl"),             # value.w, address.l
    0x22: ("text_clear", ""),
    0x23: ("end_level", ""),               # level_exit = $FF
    0x24: ("spawn_t1d", "xy"),
    0x25: ("load_tiles", "wll"),
    0x26: ("spawn_t13a", ""),
    0x27: ("spawn_t13b", ""),
    0x28: ("clear_tiles_e780", ""),        # $009EA2: remove all type $13/$14 objects, then zero 8 rows of $30 words at $E780
    0x29: ("clear_tiles_e7b0", ""),        # $009EB2: zero 8 rows of $20 words at $E7B0
    0x2A: ("copy_tiles_048ca6", ""),       # $009C4A queue tile block ROM $048CA6 through the VDP list (sub_009B14), not a spawn
    0x2B: ("spawn_t15b", ""),
    0x2C: ("spawn_t23", "ww"),
    0x2D: ("spawn_t0ea", ""),              # $00A644 type $0E (ai $00C332 hit $005CC6 hp 10), counter $FF9E08++
    0x2E: ("spawn_t24a", "w"),             # $00A666 type $24 (ai $00E166 hit $006456 hp 80); w -> +$A = $E300 + w; $FF9E0E++
    0x2F: ("spawn_t24b", "w"),             # $00A692 three type $24 at x, x+$40, x+$60 (ai $00E166 hit $006488 hp 40); w -> +$A = $E400/$E410 + w; $FF9EDA, $FFA23C = 2
    0x30: ("spawn_t0fc", "ww"),            # $00A7E8 type $0F (ai $00EA26 hit $006512 hp 4); w = x, w -> +$A
    0x31: ("spawn_t20", "w"),              # $00A80C type $20 (ai $00EA6A hit $006544 hp 4); w = x
    0x32: ("spawn_t0fd", "ww"),            # $00A8CC type $0F (ai $00EB2E hit $006576 hp 4); w = x, w = y
    0x33: ("spawn_t0fe", "ww"),            # $00A8F0 type $0F (ai $00EB62 hit $0065A4 hp 2); w = x, w = y
    0x34: ("spawn_t29", "w"),              # $00AA3E type $29 (ai $00EC58 hit $0067F2 hp 20); w = x, +$24 = $10; $FF9E12++
    0x35: ("spawn_t26a", "ww"),            # $00AA6A type $26 (ai $00EDD4 hit $00683A hp 10); w = x, w = y, +$28 = $BB; $FF9E12++
    0x36: ("spawn_t26b", "ww"),            # $00AAD2 type $26 (ai $00EFA4 hit $006894 hp 10); w = x, w = y (y<0: AI $00F69C, +$28 = -y); $FF9E12++
    0x37: ("spawn_t26c", "ww"),            # $00AB8A type $26 (ai $00F18A hit $0068CA hp 10); w = x, w = y (y<0: AI $00F6F4, +$28 = -y); $FF9E12++
    0x38: ("spawn_t23a", "ww"),            # $00AC6A type $23 (ai $00F540 hit $0067C6 hp 30); w = x, w -> +$28 (w<0: +$26 = $FFFF, +$28 = -w); $FF9E10++
    0x39: ("spawn_t27a", "ww"),            # $00ADD0 type $27 (ai $00FBAA hit $006AE0 hp 10); w = x, w = y (y<0: AI $00FE3C, +$28 = -y); $FF9E0A++
    0x3A: ("spawn_t27b", "ww"),            # $00AE72 type $27 (ai $00FF12 hit $006B32 hp 10); w = x, w = y (y<0: AI $0100D0, +$28 = -y); $FF9E0A++
    0x3B: ("spawn_t28b", ""),              # $00AD14 type $28 (ai $010238 hit $006B96 hp 400), helper $010408
    0x3C: ("spawn_t0ff", ""),              # $00ACF2 type $0F (ai $010122 hit $006B68 hp 4); $FF9EDA = object
    0x3D: ("gosub", "l"),
    0x3E: ("return", ""),
}
# native routines reached through op $14 and the number of extra arg bytes they read
NATIVE_ARGS = {0x00A2A8: 6, 0x00B35A: 2, 0x00A82C: 2, 0x00ACB0: 2}   # $00B35A DemoSkip: word offset, taken in the attract demo


def rom():
    return open(ROM, "rb").read()


def w(r, a):
    return struct.unpack(">H", r[a:a + 2])[0]


def l(r, a):
    return struct.unpack(">I", r[a:a + 4])[0]


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def walk(r, start, out, seen, stats, depth=0):
    """Decode one script (follows goto inline, gosub as a separate block)."""
    a = start
    t = 0
    while True:
        if a in seen:
            out.append(f"  {a:06x}  (joins {a:06x}: already listed)")
            return
        seen.add(a)
        d = w(r, a)
        op = w(r, a + 2)
        t += d
        p = a + 4
        if op not in OPS:
            out.append(f"  {a:06x}  +{d:<4} ?? op {op:#x}: stop")
            return
        name, lay = OPS[op]
        stats[name] = stats.get(name, 0) + 1
        args = []
        for c in lay:
            if c == "l":
                args.append(f"${l(r, p):06x}")
                p += 4
            elif c == "?":
                v = w(r, p)
                args.append(str(s16(v)))
                p += 2
                if v == 0:
                    args.append(str(s16(w(r, p))))
                    p += 2
            else:
                args.append(str(s16(w(r, p))))
                p += 2
        if name == "native":
            n = NATIVE_ARGS.get(l(r, a + 4), 0)
            for _ in range(n // 2):
                args.append(str(s16(w(r, p))))
                p += 2
        out.append(f"  {a:06x}  +{d:<4} t={t:<5} {name:<16} {' '.join(args)}")
        if name in ("end_level", "return"):
            return
        if name == "goto":
            a = l(r, a + 4)
            out.append(f"  -> {a:06x}")
            continue
        if name == "gosub":
            sub = l(r, a + 4)
            if sub not in seen:
                blk = []
                walk(r, sub, blk, seen, stats, depth + 1)
                out.append(f"     [subscript {sub:06x}]")
                out.extend("    " + x for x in blk)
        a = p


def main():
    r = rom()
    args = sys.argv[1:]
    want_stats = "--stats" in args
    levels = [int(x, 0) for x in args if not x.startswith("--")] or range(12)
    allstats = {}
    for n in levels:
        ptr = l(r, LEVELTAB + 4 * n)
        if not ptr or ptr >= len(r):
            continue
        out, stats = [], {}
        walk(r, ptr, out, set(), stats)
        allstats[n] = stats
        if not want_stats:
            print(f"level {n}: script ${ptr:06x}")
            print("\n".join(out))
    if want_stats:
        names = sorted({k for s in allstats.values() for k in s})
        print("op".ljust(18) + " ".join(f"L{n:<3}" for n in allstats))
        for k in names:
            print(k.ljust(18) + " ".join(f"{allstats[n].get(k, 0):<4}" for n in allstats))


if __name__ == "__main__":
    main()
