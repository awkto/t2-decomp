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
    0x15: ("spawn_15", "w"),
    0x16: ("text", "l"),                   # print a text record (PrintText $007DEE)
    0x17: ("draw_17", ""),
    0x18: ("poke_long", "ll"),             # value.l, address.l
    0x19: ("spawn_t1b", ""),
    0x1A: ("op_1a", ""),
    0x1B: ("spawn_t17", ""),
    0x1C: ("op_1c", ""),
    0x1D: ("spawn_t01c", "xy"),
    0x1E: ("spawn_t11", "xy"),
    0x1F: ("spawn_t28", "xy"),
    0x20: ("spawn_t1a", ""),
    0x21: ("poke_word", "wl"),             # value.w, address.l
    0x22: ("text_clear", ""),
    0x23: ("end_level", ""),               # level_exit = $FF
    0x24: ("spawn_t1d", "xy"),
    0x25: ("load_tiles", "wll"),
    0x26: ("spawn_t13a", ""),
    0x27: ("spawn_t13b", ""),
    0x28: ("op_28", ""),
    0x29: ("op_29", ""),
    0x2A: ("spawn_t15a", ""),
    0x2B: ("spawn_t15b", ""),
    0x2C: ("spawn_t23", "ww"),
    0x2D: ("op_2d", ""),
    0x2E: ("op_2e", "w"),
    0x2F: ("op_2f", "w"),
    0x30: ("op_30", "ww"),
    0x31: ("op_31", "w"),
    0x32: ("op_32", "ww"),
    0x33: ("op_33", "ww"),
    0x34: ("op_34", "w"),
    0x35: ("op_35", "ww"),
    0x36: ("op_36", "ww"),
    0x37: ("op_37", "ww"),
    0x38: ("op_38", "ww"),
    0x39: ("op_39", "ww"),
    0x3A: ("op_3a", "ww"),
    0x3B: ("op_3b", ""),
    0x3C: ("op_3c", ""),
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
