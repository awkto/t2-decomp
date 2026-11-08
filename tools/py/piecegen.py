#!/usr/bin/env python3
"""Generator for the 'pieces between jsr's' ports (port/game/ringevt.c, port/game/playerstate.c).

Reads src/t2.s through objdump (costcheck.py's assembler setup) and the GPGX opcode cycle table and emits C
for the work-RAM-only instructions of some routines, as HLE_RANGE_PURE pieces: a piece runs from an entry
(a routine start or the return address of a jsr/bsr) up to the next jsr/bsr/jmp/rts (or a branch that
leaves the routine's range) and hle_jump()s to that instruction, which the 68k executes.  The exits (with
their stack delta) are computed by a walk over the control flow.

A spec with whole=True emits each entry as ONE function (HLE_FUNC_YIELD) instead of pieces: the jsr/bsr
instructions are charged and made as direct C calls through hle_callc (the callee is C and can be
interrupted too, else it runs as nested 68k), the rts returns. For code that must run with no 68k fallback.

usage: python3 -I tools/py/piecegen.py ring|player|pickup > port/game/<file>.c"""
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import costcheck as cc

SPECS = {
    "ring": dict(
        file="ringevt.c",
        ranges=[(0x013E30, 0x014272)],
        entries={0x013E30: "EventPush", 0x013F7E: "EventRing", 0x0141B8: "EventText", 0x0141E6: "EventText2"},
        prefix="EventRing",
        title="""The event ring consumer $013F7E (docs/remaining.md "Other") and its helpers: the bonus/powerup
 * message popup that is queued as byte pairs in the ring at $FF80A0 (head $FF80B0, tail $FF80B2), the
 * producer sub_013E30 ($013E30), the text-row printers sub_0141B8 / sub_0141E6.""",
    ),
    "player": dict(
        file="playerstate.c",
        ranges=[(0x008C52, 0x008D68)],
        entries={0x008C52: "PlayersState", 0x008C8E: "PlayerState"},
        prefix="PlayerState",
        title="""The per-player state handler $008C8E (PlayerState) and its caller $008C52 (PlayersState): timers,
 * the state-1/5/7 sounds and bars, the state-2 death / continue paths up to the PlayerDied label
 * ($008D68, which game/screens.c already covers).""",
    ),
    "pickup": dict(
        file="pickup2.c",
        whole=True,
        ranges=[(0x012D00, 0x012DDC)],
        entries={0x012D00: "PickupSpawn"},
        prefix="Pickup",
        title="""The pickup/missile spawn helper sub_012D00 (called by PlayerButtons and PlayerShoot): it picks the
 * slot with MissileAlloc, fills it from the player's record and the table at $012DDC, and starts the
 * target search.""",
    ),
    "bgstream": dict(
        file="bgstream.c",
        whole=True,
        ranges=[(0x00498A, 0x004A64)],
        entries={0x00498A: "BgStreamLayers"},
        prefix="BgStream",
        title="""BgStreamLayers $00498A: one pass per background layer record (a0 = the layer list, a1/a2/a6 the
 * position tables, d0..d2 the plane/stride): when the scroll position crossed a block it queues the new
 * column/row with VdpQueueTileRect (or the Mirror / FillRows variants) after saving the registers.""",
    ),
    "queues": dict(
        file="queues2.c",
        whole=True,
        ranges=[(0x012212, 0x012234), (0x00BF46, 0x00BF60), (0x013470, 0x013480), (0x01385A, 0x01390E)],
        entries={0x012212: "VdpQueueFillRows", 0x00BF46: "VdpQueueBand", 0x013470: "SpawnRowA", 0x01385A: "SpawnRowB"},
        prefix="Queue",
        title="""Small list/spawn routines that BgStreamLayers and ScrTextClear call: VdpQueueFillRows $012212 (one
 * fill-rows command per row), the fixed list entry sub_00BF46, and the two object row spawners
 * sub_013470 / sub_01385A (they share the tail sub_013866).""",
    ),
    "helpers": dict(
        file="helpers.c",
        ranges=[(0x004820, 0x00498A), (0x00920C, 0x009242), (0x00C442, 0x00C48E), (0x00D238, 0x00D27A),
                (0x00DB36, 0x00DB5A)],
        entries={0x004820: "BgStreamL5", 0x004872: "BgStreamL2", 0x0048C4: "BgStreamL4", 0x004916: "BgStreamL1",
                 0x00920C: "HudBarHelper", 0x00C442: "AnimHelper", 0x00D238: "TruckHelper", 0x00DB36: "ObjHelper"},
        prefix="Helper",
        title="""Small helpers that were still 68k (docs/remaining.md): the per-layer background stream wrappers
 * BgStreamL5/L2/L4/L1 ($004820..$00498A, they set the layer tables and `bsr` into BgStreamLayers), the HUD bar
 * helper $00920C, the animation helper $00C442, the truck helper $00D238 and the object helper $00DB36.""",
    ),
}


class Insn:
    def __init__(self, addr, words, text):
        self.addr, self.words, self.text, self.sym = addr, words, text, []


def disassemble_r():
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
                insns.append(Insn(int(m.group(1), 16), ws, m.group(3).strip()))
            continue
        m = rr.match(ln)
        if m and insns:
            insns[-1].sym.append(m.group(1))
    return insns


def clean(t):
    return re.sub(r"\s*<[^>]*>", "", t).strip()


def resolve(i):
    t = clean(i.text)
    for sy in i.sym:
        m = re.fullmatch(r"\.text\+0x([0-9a-f]+)", sy)
        if m:
            val = int(m.group(1), 16)
        else:
            m = re.fullmatch(r"(?:dat|loc|sub)_([0-9A-Fa-f]{6})", sy)
            assert m, "reloc symbol " + sy
            val = int(m.group(1), 16)
        new, n = re.subn(r"(?<![0-9a-f%#])0(?![0-9a-f])", "%x" % val, t, count=1)
        assert n == 1, (t, sy)
        t = new
    return t


def reg(n):
    return {"fp": 6, "sp": 7}.get(n) if n in ("fp", "sp") else int(n[1])


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
    m = re.fullmatch(r"%(a\d|fp|sp)@\((-?\d+),%d(\d):([wl])\)", s)
    if m:
        return ("idx", reg(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4))
    m = re.fullmatch(r"%pc@\(([0-9a-f]+)\)", s)
    if m:
        return ("abs", int(m.group(1), 16))
    m = re.fullmatch(r"[0-9a-f]+", s)
    if m:
        return ("abs", int(s, 16))
    raise ValueError("operand " + s)


SZ = {"b": 1, "w": 2, "l": 4}
MASK = {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}
BITS = {1: 8, 2: 16, 4: 32}
CONDS = {"eq": "COND_EQ", "ne": "COND_NE", "cc": "COND_CC", "cs": "COND_CS", "hi": "COND_HI",
         "ls": "COND_LS", "pl": "COND_PL", "mi": "COND_MI", "ge": "COND_GE", "lt": "COND_LT",
         "gt": "COND_GT", "le": "COND_LE", "vc": "COND_VC", "vs": "COND_VS"}
BRANCH = re.compile(r"b(ra|eq|ne|cc|cs|hi|ls|pl|mi|ge|lt|gt|le|vc|vs)([sw])")
SHKIND = {"asl": "L", "asr": "a", "lsl": "l", "lsr": "r"}
CALLS = ("jsr", "bsrs", "bsrw", "bsr")


def lit(v):
    return "0x%08Xu" % (v & 0xFFFFFFFF)


def is_mem(op):
    return op[0] in ("ind", "post", "pre", "disp", "abs", "idx")


def ea_of(op, sz, tag):
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
    if k == "idx":
        ix = ("(uint32_t)(int32_t)(int16_t)ctx->d[%d]" if op[4] == "w" else "ctx->d[%d]") % op[3]
        return ("uint32_t %s = ctx->a[%d] + (uint32_t)(int32_t)(%d) + %s; " % (nm, op[1], op[2], ix), nm)
    if k == "abs":
        return ("uint32_t %s = 0x%08Xu; " % (nm, op[1] & 0xFFFFFFFF), nm)
    raise ValueError("ea " + str(op))


def rd_expr(op, sz, tag):
    k = op[0]
    if k == "d":
        return "(ctx->d[%d] & 0x%Xu)" % (op[1], MASK[sz])
    if k == "a":
        return "(ctx->a[%d] & 0x%Xu)" % (op[1], MASK[sz])
    if k == "i":
        return "0x%Xu" % (op[1] & MASK[sz])
    return "ld%d(ea%s)" % (BITS[sz], tag.upper())


def wr_stmt(op, sz, val, tag):
    k = op[0]
    if k == "d":
        if sz == 4:
            return "ctx->d[%d] = %s;" % (op[1], val)
        return "set_%s(&ctx->d[%d], %s);" % ("b" if sz == 1 else "w", op[1], val)
    return "st%d(ea%s, %s);" % (BITS[sz], tag.upper(), val)


def lg(sz, val):
    return "cc_logic%d(ctx, %s);" % (BITS[sz], val)


class Gen:
    def __init__(self, spec):
        self.spec = spec
        insns = disassemble_r()
        self.T = cc.load_table()
        self.ins = {}
        for i in insns:
            if any(a <= i.addr < b for a, b in spec["ranges"]):
                self.ins[i.addr] = i

    def inrange(self, a):
        return any(x <= a < y for x, y in self.spec["ranges"])

    def length(self, i):
        return 2 * len(i.words)

    def parsed(self, i):
        t = resolve(i)
        sp = t.split(None, 1)
        mn = sp[0]
        args = cc.split_args(sp[1]) if len(sp) > 1 else []
        return t, mn, args

    def kind(self, i):
        t, mn, args = self.parsed(i)
        if mn in ("rts", "jmp") or mn in CALLS:
            return "stop", []
        m = BRANCH.fullmatch(mn)
        if m:
            tgt = int(args[0], 16)
            return ("bra" if m.group(1) == "ra" else "bcc"), [tgt]
        if mn == "dbf":
            return "dbf", [int(args[1], 16)]
        return "seq", []

    def spdelta(self, i):
        t, mn, args = self.parsed(i)
        if "%sp" not in t:
            return 0
        m = re.fullmatch(r"(move|movea)([bwl])", mn)
        if m:
            sz = 4 if m.group(2) == "l" else 2
            if args[1] == "%sp@-":
                return -sz
            if args[0] == "%sp@+":
                return sz
        m = re.fullmatch(r"movem([wl])", mn)
        if m:
            sz = 4 if m.group(1) == "l" else 2
            n = bin(i.words[1]).count("1")
            if args[1] == "%sp@-":
                return -n * sz
            if args[0] == "%sp@+":
                return n * sz
        raise AssertionError("sp use %X %s" % (i.addr, t))

    def walk(self):
        entries = {}
        reached = set()
        todo = list(self.spec["entries"])
        while todo:
            e = todo.pop()
            if e in entries:
                continue
            exits = set()
            entries[e] = exits
            seen = {}
            stack = [(e, 0)]
            while stack:
                a, d = stack.pop()
                if not self.inrange(a):
                    exits.add((a, d))
                    continue
                if a in seen:
                    assert seen[a] == d, "stack mismatch at %X" % a
                    continue
                seen[a] = d
                reached.add(a)
                i = self.ins[a]
                cls, tg = self.kind(i)
                nxt = a + self.length(i)
                if cls == "stop":
                    t, mn, args = self.parsed(i)
                    if self.spec.get("whole"):
                        assert mn in CALLS or mn == "rts", "whole mode: %s at %X" % (t, a)
                        if mn in CALLS:
                            stack.append((nxt, d))
                        continue
                    exits.add((a, d))
                    if mn in CALLS:
                        todo.append(nxt)
                    continue
                d2 = d + self.spdelta(i)
                if cls == "bra":
                    stack.append((tg[0], d2))
                elif cls in ("bcc", "dbf"):
                    stack.append((tg[0], d2))
                    stack.append((nxt, d2))
                else:
                    stack.append((nxt, d2))
        return entries, reached

    def goto(self, tgt):
        return ("goto L_%06X;" % tgt) if self.inrange(tgt) else ("hle_jump(ctx, 0x%06X); return;" % tgt)

    def cost(self, i):
        ic = cc.insn_costs(i.words, self.T)
        assert ic is not None, "no cost %X" % i.addr
        return sorted(ic)[0]

    def emit(self, i, labels):
        t, mn, args = self.parsed(i)
        a = i.addr
        cmt = "/* %06X %s */" % (a, t)
        lab = "L_%06X:\n" % a if a in labels else ""
        base = self.T[i.words[0]]

        nxt = a + self.length(i)
        clsx = self.kind(i)[0]
        tail = ""
        if clsx not in ("stop", "bra") and not self.inrange(nxt):
            tail = " hle_jump(ctx, 0x%06X); return;" % nxt     # falls out of the range

        def line(s):
            return lab + "\t" + s + tail + "  " + cmt

        cls, tg = self.kind(i)
        if cls == "stop" and self.spec.get("whole"):
            if mn == "rts":
                return line("hle_insn(ctx, %d); return;" % base)
            tgt = args[0]
            if mn == "jsr" and tgt.startswith("0x"):
                tgt = tgt[2:]
            m2 = re.fullmatch(r"(?:%pc@\()?([0-9a-f]+)\)?", tgt)
            assert m2, "call target " + t
            return line("{ hle_insn(ctx, %d); hle_sync(ctx); hle_callc(ctx, 0x%06X); }" % (base, int(m2.group(1), 16)))
        if cls == "stop":
            return line("hle_jump(ctx, 0x%06X); return;" % a)
        m = BRANCH.fullmatch(mn)
        if m:
            kind, w = m.group(1), m.group(2)
            go = self.goto(tg[0])
            if kind == "ra":
                return line("hle_insn(ctx, %d); %s" % (base, go))
            nt = 8 if w == "s" else 12
            return line("if (%s) { hle_insn(ctx, 10); %s } hle_insn(ctx, %d);" % (CONDS[kind], go, nt))
        if mn == "dbf":
            r = parse_op(args[0])[1]
            return line("{ uint16_t v = (uint16_t)ctx->d[%d] - 1; set_w(&ctx->d[%d], v); if (v != 0xFFFF) { hle_insn(ctx, 10); %s } hle_insn(ctx, 14); }" % (r, r, self.goto(tg[0])))
        if mn == "moveq":
            v = parse_op(args[0])[1]
            r = parse_op(args[1])[1]
            return line("{ ctx->d[%d] = %s; cc_logic32(ctx, ctx->d[%d]); } hle_insn(ctx, %d);" % (r, lit(v & 0xFFFFFFFF), r, base))
        if mn == "moveml":
            mask = i.words[1]
            n = bin(mask).count("1")
            cost = self.cost(i)
            if args[1] == "%sp@-":
                assert not (mask & 1), "a7 in movem"
                regs = [15 - b for b in range(16) if mask & (1 << b)]
                body = "".join("ctx->a[7] -= 4; st32(ctx->a[7], %s); " % ("ctx->d[%d]" % r if r < 8 else "ctx->a[%d]" % (r - 8)) for r in regs)
                return line("{ %s} hle_movem_l(ctx, %d, %d);" % (body, cost, n))
            assert args[0] == "%sp@+"
            regs = [b for b in range(16) if mask & (1 << b)]
            body = "".join("%s = ld32(ctx->a[7]); ctx->a[7] += 4; " % ("ctx->d[%d]" % r if r < 8 else "ctx->a[%d]" % (r - 8)) for r in regs)
            return line("{ %s} hle_movem_l(ctx, %d, %d);" % (body, cost, n))
        if mn in ("st", "sf"):
            op = parse_op(args[0])
            val = "0xFF" if mn == "st" else "0"
            if op[0] == "d":
                return line("set_b(&ctx->d[%d], %s); hle_insn(ctx, %d);" % (op[1], val, base))
            pre, ea = ea_of(op, 1, "d")
            return line("{ %s st8(eaD, %s); } hle_insn(ctx, %d);" % (pre, val, base))
        if mn == "exg":
            r1, r2 = parse_op(args[0]), parse_op(args[1])
            assert r1[0] == "d" and r2[0] == "d"
            return line("{ uint32_t t_ = ctx->d[%d]; ctx->d[%d] = ctx->d[%d]; ctx->d[%d] = t_; } hle_insn(ctx, %d);" % (r1[1], r1[1], r2[1], r2[1], base))
        if mn == "lea":
            src = parse_op(args[0])
            dst = parse_op(args[1])
            pre, ea = ea_of(src, 4, "s")
            return line("{ %sctx->a[%d] = %s; } hle_insn(ctx, %d);" % (pre, dst[1], ea, base))
        if mn in ("btst", "bset", "bclr", "bchg"):
            s = parse_op(args[0])
            d = parse_op(args[1])
            if d[0] == "d":
                bitexpr = ("%d" % (s[1] & 31)) if s[0] == "i" else "(ctx->d[%d] & 31)" % s[1]
                cur = "ctx->d[%d]" % d[1]
                act = {"btst": "", "bset": "%s |= m;" % cur, "bclr": "%s &= ~m;" % cur, "bchg": "%s ^= m;" % cur}[mn]
                cost = ("%d + ((b > 15) ? 2 : 0)" % base) if mn != "btst" else "%d" % base
                return line("{ unsigned b = %s; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (%s & m) ? 0 : CC_Z); %s hle_insn(ctx, %s); }" % (bitexpr, cur, act, cost))
            pre, ea = ea_of(d, 1, "d")
            bitexpr = ("%d" % (s[1] & 7)) if s[0] == "i" else "(ctx->d[%d] & 7)" % s[1]
            act = {"btst": "", "bset": "st8(eaD, v | m);", "bclr": "st8(eaD, v & ~m);", "bchg": "st8(eaD, v ^ m);"}[mn]
            return line("{ %s unsigned b = %s; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z); %s } hle_insn(ctx, %d);" % (pre, bitexpr, act, base))
        m = re.fullmatch(r"ext([wl])", mn)
        if m:
            r = parse_op(args[0])[1]
            if m.group(1) == "w":
                return line("{ set_w(&ctx->d[%d], (uint32_t)(int32_t)(int8_t)ctx->d[%d]); cc_logic16(ctx, ctx->d[%d] & 0xFFFFu); } hle_insn(ctx, %d);" % (r, r, r, base))
            return line("{ ctx->d[%d] = (uint32_t)(int32_t)(int16_t)ctx->d[%d]; cc_logic32(ctx, ctx->d[%d]); } hle_insn(ctx, %d);" % (r, r, r, base))
        if mn == "movemw":
            mask = i.words[1]
            src = parse_op(args[0])
            assert src[0] in ("disp", "ind"), t
            pre, ea = ea_of(src, 2, "s")
            regs = [b for b in range(16) if mask & (1 << b)]
            body = "".join("%s = (uint32_t)(int32_t)(int16_t)ld16(eaS + %d); " % ("ctx->d[%d]" % r if r < 8 else "ctx->a[%d]" % (r - 8), 2 * k) for k, r in enumerate(regs))
            return line("{ %s%s} hle_insn(ctx, %d);" % (pre, body, self.cost(i)))
        m = re.fullmatch(r"(move|movea|clr|neg|tst|cmpi|cmpa|cmp|addq|subq|addi|subi|add|sub|ori|andi|eori|or|and|not|asl|asr|lsl|lsr)([bwl])", mn)
        assert m, "unhandled %s at %X" % (t, a)
        return line(self.gen_sized(i, m.group(1), SZ[m.group(2)], args, base))

    def gen_sized(self, i, kind, sz, args, base):
        ops = [parse_op(x) for x in args]
        pre = ""
        if kind in ("clr", "tst", "not", "neg"):
            src, dst = None, ops[0]
        else:
            src, dst = ops[0], ops[1]
        if kind in SHKIND:
            assert src[0] == "i" and dst[0] == "d"
            return "{ uint32_t v = shf(ctx, '%s', %s, %d, %d); %s } hle_insn(ctx, %d);" % (
                SHKIND[kind], rd_expr(dst, sz, "d"), src[1], BITS[sz], wr_stmt(dst, sz, "v", "d"), self.cost(i))
        if src is not None and is_mem(src):
            pre += ea_of(src, sz, "s")[0]
        if is_mem(dst):
            pre += ea_of(dst, sz, "d")[0]
        end = "hle_insn(ctx, %d);" % self.cost(i)
        if kind == "move":
            body = "uint32_t v = %s; %s %s" % (rd_expr(src, sz, "s"), wr_stmt(dst, sz, "v", "d"), lg(sz, "v"))
        elif kind == "movea":
            v = rd_expr(src, sz, "s")
            if sz == 2:
                v = "(uint32_t)(int32_t)(int16_t)%s" % v
            body = "ctx->a[%d] = %s;" % (dst[1], v)
        elif kind == "clr":
            body = "%s cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);" % wr_stmt(dst, sz, "0", "d")
        elif kind == "neg":
            body = "uint32_t v = alu(ctx, OP_SUB, 0, %s, %d, 1); %s" % (rd_expr(dst, sz, "d"), BITS[sz], wr_stmt(dst, sz, "v", "d"))
        elif kind == "tst":
            body = lg(sz, rd_expr(dst, sz, "d"))
        elif kind == "not":
            body = "uint32_t v = ~%s & 0x%Xu; %s %s" % (rd_expr(dst, sz, "d"), MASK[sz], wr_stmt(dst, sz, "v", "d"), lg(sz, "v"))
        elif kind in ("cmp", "cmpi"):
            body = "alu(ctx, OP_SUB, %s, %s, %d, 0);" % (rd_expr(dst, sz, "d"), rd_expr(src, sz, "s"), BITS[sz])
        elif kind == "cmpa":
            s = rd_expr(src, sz, "s")
            if sz == 2:
                s = "(uint32_t)(int32_t)(int16_t)%s" % s
            body = "alu(ctx, OP_SUB, ctx->a[%d], %s, 32, 0);" % (dst[1], s)
        elif kind in ("add", "addi", "addq", "sub", "subi", "subq"):
            if dst[0] == "a":
                # addq/subq to An: whole register, sign-extended source, no flags
                assert kind in ("addq", "subq") and src[0] == "i"
                body = "ctx->a[%d] %s= %d;" % (dst[1], "+" if kind == "addq" else "-", src[1])
                return "{ %s } %s" % (body, end)
            op = "OP_ADD" if kind.startswith("add") else "OP_SUB"
            body = "uint32_t v = alu(ctx, %s, %s, %s, %d, 1); %s" % (op, rd_expr(dst, sz, "d"), rd_expr(src, sz, "s"), BITS[sz], wr_stmt(dst, sz, "v", "d"))
        elif kind in ("or", "ori", "and", "andi", "eori"):
            o = {"o": "|", "a": "&", "e": "^"}[kind[0]]
            body = "uint32_t v = (%s %s %s) & 0x%Xu; %s %s" % (rd_expr(dst, sz, "d"), o, rd_expr(src, sz, "s"), MASK[sz], wr_stmt(dst, sz, "v", "d"), lg(sz, "v"))
        else:
            raise ValueError(kind)
        return "{ %s%s } %s" % (pre, body, end)


HEADER = r"""/* %(title)s
 *
 * These routines call back into code that waits for VBlank, prints text, spawns sprites or loads
 * level data (WaitFrames, WaitVBlank, ReadInput, PalFade, TextPrint, QueueSprite, VdpQueueTileRect, Random,
 * the sound requests): all of that stays whatever it is (C where it is registered, else the real 68k).
 * The routines are registered as HLE_RANGE_PURE pieces that cover only their OWN instructions: a piece
 * runs from an entry (the routine start, or the return address of a `jsr`/`bsr`) up to the next
 * `jsr`/`bsr`/`jmp`/`rts` and leaves with hle_jump to that instruction, which the 68k executes (so every
 * callee is entered exactly as in the original and a callee that is itself replaced is dispatched on its
 * own). A branch that leaves the routine's range is an exit as well. The pieces touch only work RAM, ROM
 * tables and registers, so the layer measures each one with a dry run and declines it when a VBlank would
 * fall inside: no hand-made bound is needed. Stack-pushing pieces declare HLE_EXIT_SP exits.
 *
 * Generated instruction by instruction from the disassembly by tools/py/piecegen.py (objdump with
 * relocations and the GPGX opcode cycle table); `costcheck.py` checks every charge. All the pieces are in
 * one function, pc_run(), entered through a switch on the entry address; branches inside the routines
 * are gotos. */
#include "hle.h"

#include <stdint.h>

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

/* asl ('L') / lsl ('l') / lsr ('r') / asr ('a') by an immediate count (1..8) on a register */
static uint32_t shf(hle_ctx *c, char kind, uint32_t v, int n, int bits)
{
	uint32_t m = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1), sg = 1u << (bits - 1);
	uint32_t res = v & m;
	unsigned cb = 0, ov = 0;
	for (int k = 0; k < n; k++) {
		if (kind == 'l' || kind == 'L') {
			cb = (res & sg) ? 1 : 0;
			res = (res << 1) & m;
			if (kind == 'L' && ((res & sg) != 0) != (cb != 0))
				ov = 1;
		} else if (kind == 'r') {
			cb = res & 1;
			res >>= 1;
		} else {
			cb = res & 1;
			res = (res >> 1) | (res & sg);
		}
	}
	unsigned f = (cb ? CC_C | CC_X : 0) | ((res & sg) ? CC_N : 0) | (res ? 0 : CC_Z) | (ov ? CC_V : 0);
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

/* work RAM or ROM by address (the code reads both: slot fields and the ROM tables) */
static inline int is_ram(uint32_t a) { return (a & 0xFFFFFFu) >= 0xE00000u; }
static inline uint8_t ld8(uint32_t a) { return is_ram(a) ? RAM8(a) : ROM8(a); }
static inline uint16_t ld16(uint32_t a) { return is_ram(a) ? RAM16(a) : ROM16(a); }
static inline uint32_t ld32(uint32_t a) { return is_ram(a) ? RAM32(a) : ROM32(a); }
static inline void st8(uint32_t a, uint32_t v) { RAM8(a) = (uint8_t)v; }
static inline void st16(uint32_t a, uint32_t v) { RAM16(a) = (uint16_t)v; }
static inline void st32(uint32_t a, uint32_t v) { RAM32_W(a, v); }

"""


def main():
    name = sys.argv[1]
    spec = SPECS[name]
    g = Gen(spec)
    entries, reached = g.walk()
    labels = set(entries)
    for a in reached:
        cls, tg = g.kind(g.ins[a])
        for tt in tg:
            if g.inrange(tt):
                labels.add(tt)
    # an entry whose first instruction is already a jsr/bsr/jmp/rts has no piece: the 68k runs it (a hook
    # there would only jump to itself)
    for e in list(entries):
        if g.kind(g.ins[e])[0] == "stop":
            del entries[e]
    out = [HEADER % dict(title=spec["title"])]
    if spec.get("whole"):
        n = 0
        for e in sorted(spec["entries"]):
            g2 = Gen(dict(spec, entries={e: spec["entries"][e]}))
            ent2, reached2 = g2.walk()
            labels2 = set(ent2)
            for a in reached2:
                cls, tg = g2.kind(g2.ins[a])
                for tt in tg:
                    if g2.inrange(tt):
                        labels2.add(tt)
            labels2.discard(e) if len(spec["entries"]) > 1 else None
            out.append("HLE_FUNC_YIELD(0x%06X, %s, 4000)\n{" % (e, spec["entries"][e]))
            for a in sorted(reached2):
                out.append(g2.emit(g2.ins[a], labels2 | ({e} if e in labels else set())))
            out.append("}\n")
            n += len(reached2)
        print("\n".join(out))
        sys.stderr.write("%d instructions\n" % n)
        return
    out.append("static void pc_run(hle_ctx *ctx, uint32_t pc)\n{\n\tswitch (pc) {")
    for e in sorted(entries):
        out.append("\tcase 0x%06X: goto L_%06X;" % (e, e))
    out.append("\tdefault: __builtin_trap();\n\t}")
    for a in sorted(reached):
        out.append(g.emit(g.ins[a], labels))
    out.append("}\n")
    for e in sorted(entries):
        nm = spec["entries"].get(e, "%s_%06X" % (spec["prefix"], e))
        ex = []
        for (xa, d) in sorted(entries[e]):
            ex.append("HLE_EXIT_SP(0x%06X, %d)" % (xa, d) if d else "0x%06X" % xa)
        out.append("HLE_RANGE_PURE(0x%06X, %s, 200, %s) { pc_run(ctx, 0x%06X); }" % (e, nm, ", ".join(ex), e))
    print("\n".join(out))
    sys.stderr.write("%d instructions, %d entries\n" % (len(reached), len(entries)))


if __name__ == "__main__":
    main()
