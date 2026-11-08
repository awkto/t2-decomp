#!/usr/bin/env python3
"""Generator for port/game/sound.c: the sound driver 68k side (src/t2.s $0FBBD0..$0FC310, $0FC408.., $0FC47A..)
-> port/game/sound.c.  Reads the ROM through objdump (costcheck.disassemble) and the GPGX opcode
cycle table.  Usage: python3 -I tools/py/sndgen.py > port/game/sound.c   (needs m68k-linux-gnu-as/objdump and
the GPGX tree for the opcode cycle table, like costcheck.py)"""
import re
import sys

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import costcheck as cc

import os, subprocess, tempfile
class Insn:
    def __init__(self, addr, words, text):
        self.addr, self.words, self.text, self.sym = addr, words, text, []


def disassemble_r():
    """like costcheck.disassemble() but with the relocations: Insn.sym = symbol of a reloc'd operand"""
    root = cc.ROOT
    with tempfile.TemporaryDirectory() as td:
        with open(os.path.join(td, "baserom.md"), "wb") as f:
            f.truncate(4 << 20)
        obj = os.path.join(td, "t2.o")
        subprocess.check_call(["m68k-linux-gnu-as", "-m68000", "--register-prefix-optional",
                               "-I", td, "-o", obj, os.path.join(root, "src", "t2.s")])
        out = subprocess.check_output(["m68k-linux-gnu-objdump", "-dr", obj], text=True)
    insns = []
    rx = re.compile(r"^\s*([0-9a-f]+):\t((?:[0-9a-f]{4} )+)\s*(.*)$")
    rr = re.compile(r"^\s*[0-9a-f]+:\s+R_68K_\w+\s+(\S+)")
    for ln in out.splitlines():
        m = rx.match(ln)
        if m:
            ws = [int(w, 16) for w in m.group(2).split()]
            if m.group(3).strip() == "" and insns:
                insns[-1].words += ws
            else:
                ins = Insn(int(m.group(1), 16), ws, m.group(3).strip())
                insns.append(ins)
            continue
        m = rr.match(ln)
        if m and insns:
            insns[-1].sym.append(m.group(1))
    return insns, {i.addr: n for n, i in enumerate(insns)}
insns, idx = disassemble_r()
T = cc.load_table()

RANGES = [(0xFBBD0, 0xFC312), (0xFC408, 0xFC438), (0xFC47A, 0xFC5CC)]
sel = [i for i in insns if any(a <= i.addr < b for a, b in RANGES)]
# contiguity check
for k in range(len(sel) - 1):
    a, b = sel[k], sel[k + 1]
    if a.addr + 2 * len(a.words) != b.addr:
        assert any(a.addr + 2 * len(a.words) == e for _, e in RANGES), "gap at %X" % a.addr
byaddr = {i.addr: i for i in sel}


def clean(t):
    return re.sub(r"\s*<[^>]*>", "", t).strip()


def resolve(i):
    """instruction text with relocated operands (dat_xxxxxx) replaced by their ROM addresses"""
    t = clean(i.text)
    for sy in i.sym:
        m = re.fullmatch(r"\.text\+0x([0-9a-f]+)", sy)
        if m:
            val = int(m.group(1), 16)
        else:
            m = re.fullmatch(r"(?:dat|loc|sub)_([0-9A-Fa-f]{6})", sy)
            assert m, "reloc symbol " + sy
            val = int(m.group(1), 16)
        # the relocated operand is printed as 0
        new, n = re.subn(r"(?<![0-9a-f%#])0(?![0-9a-f])", "%x" % val, t, count=1)
        assert n == 1, (t, sy)
        t = new
    return t


def parse_op(s):
    s = s.strip()
    m = re.fullmatch(r"%d(\d)", s)
    if m:
        return ("d", int(m.group(1)))
    m = re.fullmatch(r"%a(\d)", s)
    if m:
        return ("a", int(m.group(1)))
    if s == "%fp":
        return ("a", 6)
    if s == "%sp":
        return ("a", 7)
    if s == "%sr":
        return ("sr",)
    if s.startswith("#"):
        return ("i", int(s[1:], 0))
    m = re.fullmatch(r"%(a\d|fp|sp)@", s)
    if m:
        return ("ind", reg(m.group(1)))
    m = re.fullmatch(r"%(a\d|fp|sp)@\+", s)
    if m:
        return ("post", reg(m.group(1)))
    m = re.fullmatch(r"%(a\d|fp|sp)@-", s)
    if m:
        return ("pre", reg(m.group(1)))
    m = re.fullmatch(r"%(a\d|fp|sp)@\((-?\d+)\)", s)
    if m:
        return ("disp", reg(m.group(1)), int(m.group(2)))
    m = re.fullmatch(r"%pc@\(([0-9a-f]+)\)", s)
    if m:
        return ("abs", int(m.group(1), 16))
    m = re.fullmatch(r"[0-9a-f]+", s)
    if m:
        return ("abs", int(s, 16))
    raise ValueError("operand " + s)


def reg(n):
    return {"fp": 6, "sp": 7}.get(n) if n in ("fp", "sp") else int(n[1])


SZ = {"b": 1, "w": 2, "l": 4}
MASK = {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}
BITS = {1: 8, 2: 16, 4: 32}
SUF = {1: "u", 2: "u", 4: "u"}

CONDS = {"eq": "COND_EQ", "ne": "COND_NE", "cc": "COND_CC", "cs": "COND_CS", "hi": "COND_HI",
         "ls": "COND_LS", "pl": "COND_PL", "mi": "COND_MI", "ge": "COND_GE", "lt": "COND_LT",
         "gt": "COND_GT", "le": "COND_LE", "vc": "COND_VC", "vs": "COND_VS"}

# branch targets / entries
targets = set()
for i in sel:
    t = resolve(i)
    m = re.match(r"(b\w\w|bra|bsr|dbf)([sw]?)\s+(.*)$", t)
    # simpler: parse below again; here find hex target in branch mnemonics
    mn = t.split()[0] if t else ""
    if re.fullmatch(r"(bra|b(eq|ne|cc|cs|hi|ls|pl|mi|ge|lt|gt|le|vc|vs))[sw]", mn) or mn == "dbf":
        targets.add(int(t.split()[-1].split(",")[-1], 16))
    if re.fullmatch(r"bsr[sw]", mn):
        targets.add(int(t.split()[-1], 16))

ENTRIES = {
    0x0FBBD0: ("SndTick", "the tick wrapper: takes the Z80 bus, runs the tick body, releases"),
    0x0FBBFE: ("SndTickBody", "the tick body"),
    0x0FBDDC: ("SndChanUpdate2", "channel update, then set the 0FBDE2 result"),
    0x0FBDE2: ("SndChanDone", "copy $FF0086 to $FF006E, d0 = 1"),
    0x0FBDF0: ("SndChanUpdate3", "channel update, then clear the channel words"),
    0x0FBE18: ("SndChanUpdate", "channel update"),
    0x0FBF08: ("SndNoteFetch0", "note data fetch for the music channel set"),
    0x0FBF26: ("SndNoteFetch", "note data fetch (6-byte entries per channel)"),
    0x0FBF58: ("SndEvent", "channel event reader"),
    0x0FC0A6: ("SndClearChan", "clear the six channel word pairs"),
    0x0FC0F0: ("SndClear0F0", "clear $FF0084/$FF0082"),
    0x0FC10C: ("SndSfxTick", "sound-effect channel tick"),
    0x0FC184: ("SndSfxStop", "sfx stop request"),
    0x0FC194: ("SndSfxFetch", "sfx note data fetch"),
    0x0FC2AA: ("SndSlotFlush", "send the queued command slots to the Z80"),
    0x0FC2D6: ("SndZ80Cmd1E", "write a command block to the Z80 (d3 = $1E)"),
    0x0FC2DA: ("SndZ80Cmd", "write a command block to the Z80"),
    0x0FC408: ("SndBusReq", "Z80 BUSREQ acquire + spin"),
    0x0FC428: ("SndBusRel", "Z80 BUSREQ release"),
    0x0FC47A: ("SndZ80Cmd2", "Z80 command byte 2"),
    0x0FC488: ("SndZ80Cmd4", "Z80 command byte 4"),
    0x0FC496: ("SndUploadCheck", "driver-side upload / region flag check"),
    0x0FC4FA: ("SndFlagCheck", "channel flag check, upload to the Z80"),
    0x0FC59C: ("SndWaitZ80", "wait until the Z80 command byte is 0"),
}
# already registered in vblank.c (same code, run here only as callee)
SKIP_REG = {0x0FC0FE, 0x0FC27C, 0x0FC290}
targets |= set(ENTRIES)

out = []
E = out.append


def lit(v):
    return "0x%08Xu" % (v & 0xFFFFFFFF)


def opcost(i, extra=None):
    ic = cc.insn_costs(i.words, T)
    assert ic is not None, "no cost %X" % i.addr
    return ic


def emit_insn(i):
    t = resolve(i)
    sp = t.split(None, 1)
    mn = sp[0]
    args = cc.split_args(sp[1]) if len(sp) > 1 else []
    a = i.addr
    nxt = a + 2 * len(i.words)
    cmt = "/* %06X %s */" % (a, t)
    lab = "L_%06X:\n" % a if a in targets else ""
    base = T[i.words[0]]

    def line(s):
        return lab + "\t" + s + "  " + cmt

    # ---- no-operand
    if mn == "nop":
        return line("hle_insn(ctx, %d);" % base)
    if mn == "rts":
        return line("hle_insn(ctx, %d); if (nested) ctx->a[7] += 4; return;" % base)
    m = re.fullmatch(r"b(ra|sr|eq|ne|cc|cs|hi|ls|pl|mi|ge|lt|gt|le|vc|vs)([sw])", mn)
    if m:
        kind, w = m.group(1), m.group(2)
        tgt = int(args[0], 16)
        if kind == "sr":
            assert base == 18
            return line("{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x%06Xu); } hle_insn(ctx, 18); snd_run(ctx, 0x%06Xu, 1);" % (nxt, tgt))
        if kind == "ra":
            return line("hle_insn(ctx, %d); goto L_%06X;" % (base, tgt))
        nt = 8 if w == "s" else 12
        return line("if (%s) { hle_insn(ctx, 10); goto L_%06X; } hle_insn(ctx, %d);" % (CONDS[kind], tgt, nt))
    if mn == "dbf":
        r = parse_op(args[0])[1]
        tgt = int(args[1], 16)
        return line("{ uint16_t v = (uint16_t)ctx->d[%d] - 1; set_w(&ctx->d[%d], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_%06X; } hle_insn(ctx, 14); }" % (r, r, tgt))
    if mn == "moveq":
        v = parse_op(args[0])[1]
        r = parse_op(args[1])[1]
        v32 = v & 0xFFFFFFFF
        return line("{ ctx->d[%d] = %s; cc_logic32(ctx, ctx->d[%d]); } hle_insn(ctx, %d);" % (r, lit(v32), r, base))
    if mn == "moveml":
        mask = i.words[1]
        n = bin(mask).count("1")
        cost = list(opcost(i))[0]
        if args[1] == "%sp@-":
            regs = [15 - b for b in range(16) if mask & (1 << b)]   # a7 first
            body = "".join("ctx->a[7] -= 4; w32(ctx->a[7], %s); " % ("ctx->d[%d]" % r if r < 8 else "ctx->a[%d]" % (r - 8)) for r in regs)
            return line("hle_movem_l(ctx, %d, %d); { %s}" % (cost, n, body))
        assert args[0] == "%sp@+"
        regs = [b for b in range(16) if mask & (1 << b)]
        body = "".join("%s = r32(ctx->a[7]); ctx->a[7] += 4; " % ("ctx->d[%d]" % r if r < 8 else "ctx->a[%d]" % (r - 8)) for r in regs)
        return line("hle_movem_l(ctx, %d, %d); { %s}" % (cost, n, body))

    # ---- sized instructions
    m = re.fullmatch(r"(move|movea|clr|tst|cmpi|cmp|addq|subq|addi|subi|add|sub|adda|ori|andi|or|and|lsr|lsl|roxr|mulu)([bwl])", mn)
    if mn in ("st", "sf"):
        op = parse_op(args[0])
        return line(gen_scc(i, mn, op, base))
    if mn == "lea":
        src = parse_op(args[0])
        dst = parse_op(args[1])
        pre, ea = ea_of(src, 4, "s")
        return line("{ %sctx->a[%d] = %s; } hle_insn(ctx, %d);" % (pre, dst[1], ea, base))
    if mn == "btst":
        s = parse_op(args[0])
        d = parse_op(args[1])
        if s[0] == "i":
            bit = s[1]
        else:
            raise ValueError("btst reg")
        if d[0] == "d":
            return line("{ cc_set(ctx, CC_Z, ((ctx->d[%d] >> %d) & 1) ? 0 : CC_Z); } hle_insn(ctx, %d);" % (d[1], bit & 31, base))
        pre, ea = ea_of(d, 1, "d")
        return line("{ %s int io = !plain(eaD); if (io) hle_pre(ctx); uint8_t v = r8(eaD); cc_set(ctx, CC_Z, ((v >> %d) & 1) ? 0 : CC_Z); if (io) hle_post(ctx, %d); else hle_insn(ctx, %d); }" % (pre, bit & 7, base, base))
    assert m, "unhandled %s at %X" % (t, a)
    kind, sz = m.group(1), SZ[m.group(2)]
    return line(gen_sized(i, kind, sz, args, base))


def ea_of(op, sz, tag):
    """-> (pre-statements declaring ea<tag>, address expression name)"""
    nm = "ea" + tag.upper()
    k = op[0]
    if k == "ind":
        return ("uint32_t %s = ctx->a[%d]; " % (nm, op[1]), nm)
    if k == "post":
        inc = 2 if (op[1] == 7 and sz == 1) else sz
        return ("uint32_t %s = ctx->a[%d]; ctx->a[%d] += %d; " % (nm, op[1], op[1], inc), nm)
    if k == "pre":
        dec = 2 if (op[1] == 7 and sz == 1) else sz
        return ("ctx->a[%d] -= %d; uint32_t %s = ctx->a[%d]; " % (op[1], dec, nm, op[1]), nm)
    if k == "disp":
        return ("uint32_t %s = ctx->a[%d] + (uint32_t)(int32_t)(%d); " % (nm, op[1], op[2]), nm)
    if k == "abs":
        return ("uint32_t %s = 0x%08Xu; " % (nm, op[1] & 0xFFFFFFFF), nm)
    raise ValueError("ea " + str(op))


def is_mem(op):
    return op[0] in ("ind", "post", "pre", "disp", "abs")


def rd_expr(op, sz, tag):
    k = op[0]
    if k == "d":
        return "(ctx->d[%d] & 0x%Xu)" % (op[1], MASK[sz])
    if k == "a":
        return "(ctx->a[%d] & 0x%Xu)" % (op[1], MASK[sz])
    if k == "i":
        return "0x%Xu" % (op[1] & MASK[sz])
    return "r%d(ea%s)" % (BITS[sz], tag.upper())


def wr_stmt(op, sz, val, tag):
    k = op[0]
    if k == "d":
        if sz == 4:
            return "ctx->d[%d] = %s;" % (op[1], val)
        return "set_%s(&ctx->d[%d], %s);" % ("b" if sz == 1 else "w", op[1], val)
    return "w%d(ea%s, %s);" % (BITS[sz], tag.upper(), val)


def lg(sz, val):
    return "cc_logic%d(ctx, %s);" % (BITS[sz], val)


def gen_scc(i, mn, op, base):
    pre, ea = ea_of(op, 1, "d") if is_mem(op) else ("", None)
    val = "0xFF" if mn == "st" else "0"
    if op[0] == "d":
        return "{ set_b(&ctx->d[%d], %s); } hle_insn(ctx, %d);" % (op[1], val, base)
    return "{ %s int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, %s); if (io) hle_post(ctx, %d); else hle_insn(ctx, %d); }" % (pre, val, base, base)


def gen_sized(i, kind, sz, args, base):
    ops = [parse_op(x) for x in args]
    pre = ""
    ios = []
    # source / dest effective addresses
    if kind in ("clr", "tst") or kind in ("lsr", "lsl", "roxr") and len(ops) == 1:
        src, dst = None, ops[0]
    else:
        src, dst = ops[0], ops[1]
    if kind in ("lsr", "lsl", "roxr"):
        assert src[0] == "i" and dst[0] == "d"
        n = src[1]
        k = {"lsr": 0, "lsl": 1, "roxr": 2}[kind]
        return "{ uint32_t v = shf(ctx, %d, %s, %d, %d); %s } hle_insn(ctx, %d);" % (
            k, rd_expr(dst, sz, "d"), n, BITS[sz], wr_stmt(dst, sz, "v", "d"), list(opcost(i))[0])
    if kind == "mulu":
        assert src[0] == "i" and dst[0] == "d"
        s = src[1] & 0xFFFF
        n = bin(s).count("1")
        cost = 38 + 2 * n + 4
        return "{ uint32_t res = (ctx->d[%d] & 0xFFFFu) * 0x%Xu; ctx->d[%d] = res; cc_logic32(ctx, res); } hle_insn(ctx, %d);" % (dst[1], s, dst[1], cost)
    # memory operands
    if src is not None and is_mem(src):
        p, _ = ea_of(src, sz, "s")
        pre += p
        ios.append("eaS")
    if is_mem(dst):
        p, _ = ea_of(dst, sz, "d")
        pre += p
        ios.append("eaD")
    io = ("int io = %s; if (io) hle_pre(ctx); " % " || ".join("!plain(%s)" % e for e in ios)) if ios else ""
    cost = base
    if kind == "adda":
        assert dst[0] == "a"
    end = ("if (io) hle_post(ctx, %d); else hle_insn(ctx, %d);" % (cost, cost)) if ios else "hle_insn(ctx, %d);" % cost
    if kind == "move":
        if src[0] == "sr":
            body = "%s" % wr_stmt(dst, 2, "ctx->sr", "d")
        elif dst[0] == "sr":
            body = "{ ctx->sr = (uint16_t)(%s & 0xA71Fu); }" % rd_expr(src, 2, "s")
        else:
            body = "uint32_t v = %s; %s %s" % (rd_expr(src, sz, "s"), wr_stmt(dst, sz, "v", "d"), lg(sz, "v"))
    elif kind == "movea":
        v = rd_expr(src, sz, "s")
        if sz == 2:
            v = "(uint32_t)(int32_t)(int16_t)%s" % v
        body = "ctx->a[%d] = %s;" % (dst[1], v)
    elif kind == "clr":
        body = "%s cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);" % wr_stmt(dst, sz, "0", "d")
    elif kind == "tst":
        body = lg(sz, rd_expr(dst, sz, "d"))
    elif kind in ("cmp", "cmpi"):
        body = "alu(ctx, OP_SUB, %s, %s, %d, 0);" % (rd_expr(dst, sz, "d"), rd_expr(src, sz, "s"), BITS[sz])
    elif kind in ("add", "addi", "addq", "sub", "subi", "subq"):
        assert dst[0] != "a"
        op = "OP_ADD" if kind.startswith("add") else "OP_SUB"
        body = "uint32_t v = alu(ctx, %s, %s, %s, %d, 1); %s" % (op, rd_expr(dst, sz, "d"), rd_expr(src, sz, "s"), BITS[sz], wr_stmt(dst, sz, "v", "d"))
    elif kind == "adda":
        v = rd_expr(src, sz, "s")
        if sz == 2:
            v = "(uint32_t)(int32_t)(int16_t)%s" % v
        body = "ctx->a[%d] += %s;" % (dst[1], v)
    elif kind in ("or", "ori", "and", "andi"):
        o = "|" if kind.startswith("or") else "&"
        body = "uint32_t v = (%s %s %s) & 0x%Xu; %s %s" % (rd_expr(dst, sz, "d"), o, rd_expr(src, sz, "s"), MASK[sz], wr_stmt(dst, sz, "v", "d"), lg(sz, "v"))
    else:
        raise ValueError(kind)
    return "{ %s%s%s %s }" % (pre, io, body, end)


# ------------------------------------------------------------------------ emit
E("""/* The 68k side of the sound driver: the per-frame tick $0FBBD0 (called from VBlankInt) and everything it
 * reaches: the tick body $0FBBFE, the channel update $0FBE18 / $0FBDDC / $0FBDF0, the note data fetch
 * $0FBF26 / $0FBF08, the channel event reader $0FBF58, the sfx tick $0FC10C / $0FC194 / $0FC184, the
 * command slots $0FC2AA / $0FC2D6 / $0FC2DA, the Z80 bus request / release $0FC408 / $0FC428 (with the
 * BUSACK spin), the Z80 command helpers $0FC47A / $0FC488 / $0FC496 / $0FC4FA and the wait for the
 * Z80's command byte $0FC59C.
 *
 * Generated instruction by instruction from the disassembly (tools/py/sndgen.py reading the ROM
 * through objdump and the GPGX opcode cycle table; tools/py/costcheck.py checks every charge). All the
 * routines are one function, snd_run(), entered through a switch on the entry address; a `bsr` is a
 * recursive call (the return address is pushed and popped like the original so the stack bytes are
 * the same), `rts` returns, and the branches are gotos, so a fall-through or tail `bra` into another
 * routine is just code order.
 *
 * Every instruction goes through the timeline (hle_insn / hle_pre + hle_post). The Z80 area, the I/O
 * area ($A10001 version, $A11100 BUSREQ) and the VDP are touched through bus_r8..bus_w32 between
 * hle_pre() and hle_post(), once per original instruction and in the original order: the Z80 bus
 * request makes GPGX run the Z80 up to the 68k clock, a Z80 read of the 68k bank stalls the 68k, and
 * the spin loops (BUSACK in $0FC408, the command byte in $0FC59C) are iterated one pass at a time
 * with their own charges. Work RAM and ROM are read and written directly. Whether an access is I/O
 * is decided from the effective address at run time (a6 is the Z80 RAM base read from $FF0000).
 *
 * The functions are not pure (Z80 bus), so the layer judges them by their declared bound; they are
 * always entered with the interrupt mask at 6 (inside VBlankInt), so only the frame-end check uses
 * it. */
#include "hle.h"

#include <stdint.h>

/* longest span of a call (68k cycles); only the frame-end check uses it, see above */
#define SND_BOUND 6000   /* observed max 5356 (SndTick in the difftest sandbox, no Z80 stalls) */

enum { OP_ADD, OP_SUB };

static uint32_t alu(hle_ctx *c, int op, uint32_t a, uint32_t b, int bits, int setx)
{
	uint32_t m = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1), sg = 1u << (bits - 1);
	a &= m; b &= m;
	uint32_t r;
	unsigned cy, ov;
	if (op == OP_ADD) {
		uint64_t w = (uint64_t)a + b;
		r = (uint32_t)w & m;
		cy = (unsigned)((w >> bits) & 1);
		ov = (((a ^ r) & (b ^ r)) & sg) != 0;
	} else {
		r = (a - b) & m;
		cy = a < b;
		ov = (((a ^ b) & (a ^ r)) & sg) != 0;
	}
	unsigned f = (cy ? CC_C : 0) | (ov ? CC_V : 0) | ((r & sg) ? CC_N : 0) | (r ? 0 : CC_Z);
	if (setx && cy)
		f |= CC_X;
	cc_set(c, (setx ? CC_X : 0) | CC_N | CC_Z | CC_V | CC_C, f);
	return r;
}

/* lsr / lsl / roxr by an immediate count (1..8); returns the result, sets X N Z V C */
static uint32_t shf(hle_ctx *c, int kind, uint32_t v, int n, int bits)
{
	uint32_t m = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1), sg = 1u << (bits - 1);
	uint32_t res = v & m;
	unsigned cb = 0, x = (c->sr & CC_X) ? 1 : 0;
	for (int k = 0; k < n; k++) {
		if (kind == 0) {            /* lsr */
			cb = res & 1;
			res >>= 1;
			x = cb;
		} else if (kind == 1) {     /* lsl */
			cb = (res & sg) ? 1 : 0;
			res = (res << 1) & m;
			x = cb;
		} else {                    /* roxr */
			cb = res & 1;
			res = (res >> 1) | (x ? sg : 0);
			x = cb;
		}
	}
	unsigned f = (cb ? CC_C : 0) | ((res & sg) ? CC_N : 0) | (res ? 0 : CC_Z);
	if (kind == 2 || n > 0)
		f |= x ? CC_X : 0;
	cc_set(c, CC_X | CC_N | CC_Z | CC_V | CC_C, f);
	return res;
}

#define COND_EQ ((ctx->sr & CC_Z) != 0)
#define COND_NE ((ctx->sr & CC_Z) == 0)
#define COND_CC ((ctx->sr & CC_C) == 0)
#define COND_CS ((ctx->sr & CC_C) != 0)
#define COND_PL ((ctx->sr & CC_N) == 0)
#define COND_MI ((ctx->sr & CC_N) != 0)
#define COND_VC ((ctx->sr & CC_V) == 0)
#define COND_VS ((ctx->sr & CC_V) != 0)
#define COND_HI ((ctx->sr & (CC_C | CC_Z)) == 0)
#define COND_LS ((ctx->sr & (CC_C | CC_Z)) != 0)
#define COND_GE (((ctx->sr & CC_N) != 0) == ((ctx->sr & CC_V) != 0))
#define COND_LT (((ctx->sr & CC_N) != 0) != ((ctx->sr & CC_V) != 0))
#define COND_GT (!(ctx->sr & CC_Z) && COND_GE)
#define COND_LE (((ctx->sr & CC_Z) != 0) || COND_LT)

/* work RAM and ROM are plain memory; anything else (Z80 area, I/O, VDP) is an I/O access */
static inline int plain(uint32_t a)
{
	a &= 0xFFFFFFu;
	return a >= 0xE00000u || a < 0x400000u;
}
static inline uint8_t r8(uint32_t a)
{
	uint32_t p = a & 0xFFFFFFu;
	if (p >= 0xE00000u) return RAM8(p);
	if (p < 0x400000u) return ROM8(p);
	return bus_r8(p);
}
static inline uint16_t r16(uint32_t a)
{
	uint32_t p = a & 0xFFFFFFu;
	if (p >= 0xE00000u) return RAM16(p);
	if (p < 0x400000u) return ROM16(p);
	return bus_r16(p);
}
static inline uint32_t r32(uint32_t a)
{
	uint32_t p = a & 0xFFFFFFu;
	if (p >= 0xE00000u) return RAM32(p);
	if (p < 0x400000u) return ROM32(p);
	return bus_r32(p);
}
static inline void w8(uint32_t a, uint32_t v)
{
	uint32_t p = a & 0xFFFFFFu;
	if (p >= 0xE00000u) RAM8(p) = (uint8_t)v; else bus_w8(p, (uint8_t)v);
}
static inline void w16(uint32_t a, uint32_t v)
{
	uint32_t p = a & 0xFFFFFFu;
	if (p >= 0xE00000u) RAM16(p) = (uint16_t)v; else bus_w16(p, (uint16_t)v);
}
static inline void w32(uint32_t a, uint32_t v)
{
	uint32_t p = a & 0xFFFFFFu;
	if (p >= 0xE00000u) RAM32_W(p, v); else bus_w32(p, v);
}

/* `nested` is 1 for a routine reached by a bsr of this code (its rts pops the return address the
 * bsr pushed); the top-level entry leaves its own return address to the layer. */
static void snd_run(hle_ctx *ctx, uint32_t entry, int nested)
{
	switch (entry) {""")
for e in sorted(targets):
    if e in byaddr:
        E("\tcase 0x%06X: goto L_%06X;" % (e, e))
E("\tdefault: __builtin_trap();\n\t}")
prev_end = None
for i in sel:
    if prev_end is not None and i.addr != prev_end:
        E("\t/* ---- gap ---- */")
    E(emit_insn(i))
    prev_end = i.addr + 2 * len(i.words)
E("}\n")

E("""/* the bound is the longest span of a call (the layer declines a call that could straddle the frame end) */""")
for a, (nm, what) in sorted(ENTRIES.items()):
    if a in SKIP_REG:
        continue
    E("HLE_FUNC(0x%06X, %s, SND_BOUND) { snd_run(ctx, 0x%06Xu, 0); }  /* %s */" % (a, nm, a, what))
print("\n".join(out))
