#!/usr/bin/env python3
"""Generator for port/game/glue.c: the control-transfer instructions between C pieces as C blocks
(t2-decomp#11, docs/native.md).

The pieces of earlier waves (HLE_RANGE_PURE, piecegen.py) stop in front of a jsr/bsr/jmp/rts and let the 68k execute
it. A native runner has no 68k, so each such instruction is a block of its own: it costs what the 68000 charges, does
what the instruction does (push the return address, jump, pop PC/SR) and leaves through hle_jump, so the callee and
the code at the return address are C blocks. With the emulated CPU the block replaces the same instruction with the
same charge (an exact instruction timeline, hle_insn), so lockstep is unchanged.

    python3 -I tools/py/gluegen.py [runs/ipprof ...] > port/game/glue.c

Sites: data/glue_sites.txt (committed, one hex address per line) plus every jsr/bsr/jmp/rts/rte/bra the
profiles of port/tools/ipprof.sh interpreted; addresses that already have a C block (hle-list.txt of the profile
directory) are skipped. --add writes the merged site list back to data/glue_sites.txt.
"""
import glob
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

JSR_COST = {2: 16, 5: 18, 6: 22, "w": 18, "l": 20, "pcd": 18, "pci": 22}
JMP_COST = {2: 8, 5: 10, 6: 14, "w": 10, "l": 12, "pcd": 10, "pci": 14}


def rom_words(rom, a, n):
    return [(rom[a + 2 * i] << 8) | rom[a + 2 * i + 1] for i in range(n)]


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def decode(rom, a):
    """-> dict(kind, len, cost, target (int or None), expr (C expression for a dynamic target), text)"""
    w = rom_words(rom, a, 1)[0]
    if w == 0x4E75:
        return dict(kind="rts", len=2, cost=16, text="rts")
    if w == 0x4E73:
        return dict(kind="rte", len=2, cost=20, text="rte")
    if (w & 0xFF00) == 0x6100 or (w & 0xFF00) == 0x6000:
        kind = "bsr" if (w & 0xFF00) == 0x6100 else "bra"
        d = w & 0xFF
        if d == 0:
            ln, disp = 4, s16(rom_words(rom, a + 2, 1)[0])
        elif d == 0xFF:
            return None
        else:
            ln, disp = 2, d - 256 if d & 0x80 else d
        return dict(kind=kind, len=ln, cost=18 if kind == "bsr" else 10, target=(a + 2 + disp) & 0xFFFFFF,
                    text="%s $%06X" % (kind, (a + 2 + disp) & 0xFFFFFF))
    if (w & 0xFF80) == 0x4E80:
        kind = "jsr" if (w & 0x40) == 0 else "jmp"
        kind = "jsr" if (w & 0xFFC0) == 0x4E80 else "jmp"
        mode, r = (w >> 3) & 7, w & 7
        cost = JSR_COST if kind == "jsr" else JMP_COST
        ext = rom_words(rom, a + 2, 2)
        if mode == 2:
            return dict(kind=kind, len=2, cost=cost[2], expr="ctx->a[%d]" % r, text="%s (a%d)" % (kind, r))
        if mode == 5:
            d = s16(ext[0])
            return dict(kind=kind, len=4, cost=cost[5], expr="ctx->a[%d] + (uint32_t)(int32_t)(%d)" % (r, d),
                        text="%s %d(a%d)" % (kind, d, r))
        if mode == 6 or (mode == 7 and r == 3):
            e = ext[0]
            d8 = e & 0xFF
            d8 = d8 - 256 if d8 & 0x80 else d8
            xr = (e >> 12) & 7
            xreg = ("ctx->a[%d]" if e & 0x8000 else "ctx->d[%d]") % xr
            if not e & 0x800:
                xreg = "(uint32_t)(int32_t)(int16_t)%s" % xreg
            base = "ctx->a[%d]" % r if mode == 6 else "0x%06Xu" % (a + 2)
            return dict(kind=kind, len=4, cost=cost[6], expr="%s + (uint32_t)(int32_t)(%d) + %s" % (base, d8, xreg),
                        text="%s (%d,%s,x)" % (kind, d8, "a%d" % r if mode == 6 else "pc"))
        if mode == 7 and r == 0:
            t = s16(ext[0]) & 0xFFFFFF
            return dict(kind=kind, len=4, cost=cost["w"], target=t, text="%s $%06X.w" % (kind, t))
        if mode == 7 and r == 1:
            t = ((ext[0] << 16) | ext[1]) & 0xFFFFFF
            return dict(kind=kind, len=6, cost=cost["l"], target=t, text="%s $%06X.l" % (kind, t))
        if mode == 7 and r == 2:
            t = (a + 2 + s16(ext[0])) & 0xFFFFFF
            return dict(kind=kind, len=4, cost=cost["pcd"], target=t, text="%s $%06X(pc)" % (kind, t))
    return None


def emit(rom, a, ins):
    n = "glue_%06X" % a
    k = ins["kind"]
    cost = ins["cost"]
    ret = a + ins["len"]
    tgt = ins.get("target")
    c = []
    if k == "rts":
        c.append("HLE_FUNC(0x%06X, %s, %d) { hle_insn(ctx, %d); }   /* rts */" % (a, n, cost, cost))
    elif k == "rte":
        c.append("HLE_FUNC(0x%06X, %s, %d)   /* rte */\n{\n\thle_insn(ctx, %d);\n\tctx->sr = pop16(ctx);\n"
                 "\thle_jump(ctx, pop32(ctx));\n}" % (a, n, cost, cost))
    elif k in ("jsr", "bsr"):
        body = "\thle_insn(ctx, %d);\n" % cost
        if tgt is not None:
            body += "\tpush32(ctx, 0x%06X);\n\thle_jump(ctx, 0x%06X);\n" % (ret, tgt)
            c.append("HLE_RANGE(0x%06X, %s, %d, HLE_EXIT_SP(0x%06X, -4))   /* %s */\n{\n%s}" %
                     (a, n, cost, tgt, ins["text"], body))
        else:
            body += "\tuint32_t t = %s;\n\tpush32(ctx, 0x%06X);\n\thle_jump(ctx, t);\n" % (ins["expr"], ret)
            c.append("HLE_FUNC(0x%06X, %s, %d)   /* %s */\n{\n%s}" % (a, n, cost, ins["text"], body))
    else:   # jmp / bra
        body = "\thle_insn(ctx, %d);\n" % cost
        if tgt is not None:
            body += "\thle_jump(ctx, 0x%06X);\n" % tgt
            c.append("HLE_RANGE(0x%06X, %s, %d, 0x%06X)   /* %s */\n{\n%s}" % (a, n, cost, tgt, ins["text"], body))
        else:
            body += "\thle_jump(ctx, %s);\n" % ins["expr"]
            c.append("HLE_FUNC(0x%06X, %s, %d)   /* %s */\n{\n%s}" % (a, n, cost, ins["text"], body))
    return "\n".join(c)


def main():
    args = [x for x in sys.argv[1:] if not x.startswith("--")]
    sites_path = os.path.join(ROOT, "data/glue_sites.txt")
    sites = set()
    if os.path.exists(sites_path):
        for ln in open(sites_path):
            ln = ln.split("#")[0].strip()
            if ln:
                sites.add(int(ln, 16))
    rom = open(os.path.join(ROOT, "rom/baserom.md"), "rb").read()
    for d in args or [os.path.join(ROOT, "runs/ipprof")]:
        # written by remaining68k.py (barrier insns between C pieces) and piecegen.py auto (the exits of its pieces)
        for cp in glob.glob(os.path.join(d, "glue_candidates*.txt")):
            for ln in open(cp):
                if ln.strip():
                    sites.add(int(ln.split()[0], 16))
        hl = os.path.join(d, "hle-list.txt")
        other = set()
        if os.path.exists(hl):
            for ln in open(hl):
                for m in re.finditer(r"(\S+) \(\$([0-9A-Fa-f]{6})\)", ln):
                    if not m.group(1).startswith("glue_"):
                        other.add(int(m.group(2), 16))
        sites -= other      # somebody else's block already sits there
    # entries of other files (pieces, functions): never register a second block at the same address
    for p in glob.glob(os.path.join(ROOT, "port/game/*.c")):
        if os.path.basename(p) == "glue.c":
            continue
        for m in re.finditer(r"\bHLE_(?:FUNC|RANGE|WAIT_FUNC|WAIT_RANGE)\w*\(\s*0x([0-9A-Fa-f]+)\s*,", open(p).read()):
            sites.discard(int(m.group(1), 16))
    out = []
    n = 0
    for a in sorted(sites):
        ins = decode(rom, a)
        if not ins:
            sys.stderr.write("gluegen: cannot decode $%06X\n" % a)
            continue
        out.append(emit(rom, a, ins))
        n += 1
    if "--add" in sys.argv:
        with open(sites_path, "w") as f:
            f.write("# control-transfer instructions that are C blocks (tools/py/gluegen.py): one hex address per line\n")
            for a in sorted(sites):
                f.write("%06X\n" % a)
    print("""/* Generated by tools/py/gluegen.py from data/glue_sites.txt -- do not edit.
 *
 * (not HLE_F_PURE: a dry run copies work RAM, too slow for blocks inside tight loops)
 * The control-transfer instructions (jsr, bsr, jmp, bra, rts, rte) at the ends of the C pieces, as blocks of
 * their own (t2-decomp#11, docs/native.md "Glue"): the cycles of the 68000 instruction, its effect on the stack and
 * PC, and hle_jump to the target, so a callee and the code at the return address are C blocks. Static targets are
 * HLE_RANGEs (difftest runs the original instruction up to the declared exit); computed targets (jsr (a0), jmp
 * (a0)) are functions that jump. %d blocks. */
#include "hle.h"

#include <stdint.h>
""" % n)
    print("\n".join(x + "\n" for x in out))


if __name__ == "__main__":
    main()
