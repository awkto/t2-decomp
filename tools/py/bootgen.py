#!/usr/bin/env python3
"""Generator for port/game/boot.c: the cold-boot path as C (t2-decomp#11, phase P5, docs/native.md "Cold boot").

Reset ($000200), the start of Boot ($007406..), the sound-driver boot code that loads the Z80
(sub_0FBB00, sub_0FC32E/350/3E6/438) and the small hardware helpers (VdpSetRegs, CramClear, VsramClear, ...):
the 68k code that touches the machine's hardware (VDP ports, Z80 bus and RAM, I/O ports).

Same pieces as piecegen.py (an entry, up to the next jsr/bsr/jmp/rts, which the glue blocks of glue.c execute), but

  * every instruction goes through the instruction timeline like the sound driver (game/sound.c): `hle_insn` for
    the ones that touch work RAM / ROM only, `hle_pre` .. `hle_post` around the ones that reach the I/O area, the
    Z80 or the VDP, so the access happens at the clock the 68k core would give it;
  * a backward branch is an exit of the piece (hle_jump to the loop head): one pass of a loop is one block, so a
    block never spans more than a few dozen cycles and the layer can hand any single pass to the 68k when a
    frame-loop event falls in it (the 64 KiB work-RAM clear of Reset would otherwise be one call of 3.6 frames);
  * a write to SR ends a piece (the interrupt mask may change: a pending interrupt is taken after it).

usage: python3 -I tools/py/bootgen.py > port/game/boot.c
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import piecegen as pg
from piecegen import parse_op, is_mem, ea_of, rd_expr, wr_stmt, lg, SZ, MASK, BITS, CONDS, BRANCH

# --- operand forms piecegen does not know: (d8,pc,Xn) as objdump prints it, `%pc@(ABS,%dN:w|l)` with ABS already resolved
_parse_op = pg.parse_op
_ea_of = pg.ea_of
_is_mem = pg.is_mem


def parse_op(s):
    m = re.fullmatch(r"%pc@\(([0-9a-f]+),%d(\d):([wl])\)", s.strip())
    if m:
        return ("pcidx", int(m.group(1), 16), int(m.group(2)), m.group(3))
    m = re.fullmatch(r"%(a\d|fp|sp)@\(([0-9a-f]{9,16}),%d(\d):([wl])\)", s.strip())      # a negative displacement printed as unsigned hex
    if m:
        v = int(m.group(2), 16)
        return ("idx", pg.reg(m.group(1)), v - (1 << 64) if v >= 1 << 63 else v, int(m.group(3)), m.group(4))
    if s.strip() == "%ccr":
        return ("ccr",)
    return _parse_op(s)


def is_mem(op):
    return op[0] == "pcidx" or _is_mem(op)


def ea_of(op, sz, tag):
    if op[0] == "pcidx":
        nm = "ea" + tag.upper()
        ix = ("(uint32_t)(int32_t)(int16_t)ctx->d[%d]" if op[3] == "w" else "ctx->d[%d]") % op[2]
        return ("uint32_t %s = 0x%08Xu + %s; " % (nm, op[1], ix), nm)
    return _ea_of(op, sz, tag)


pg.parse_op, pg.is_mem, pg.ea_of = parse_op, is_mem, ea_of
_resolve = pg.resolve


def resolve(i):
    try:
        return _resolve(i)
    except AssertionError:                 # a relocated branch/jmp whose target objdump already printed
        return pg.clean(i.text)


pg.resolve = resolve

SPEC = dict(
    ranges=[(0x000200, 0x00028E), (0x0002FA, 0x000306),         # Reset .. bra loc_0002FA, tst.w vdp, jmp Boot
            (0x007406, 0x00745C),                                # Boot
            (0x0FBB00, 0x0FBB6C),                                # sub_0FBB00: Z80 / sound boot
            (0x0FC32E, 0x0FC408),                                # Z80 reset, loader stub + driver upload
            (0x0FC438, 0x0FC47A),                                # sub_0FC438
            (0x00867A, 0x00869C),                                # VdpSetRegs
            (0x0086F6, 0x00873A),                                # VramClearAlt, VramClear (falls into sub_00873A)
            (0x008162, 0x0081DA),                                # CramClear, VsramClear, sub_0081B6 (not CramUpload*)
            (0x001BE8, 0x001C70),
            (0x00884C, 0x008876),                                # InitPort1                                # ReadPort2Id, sub_001BFA
            ],
    entries={0x000200: "Reset", 0x007406: "Boot", 0x0FBB00: "SndBoot", 0x0FC32E: "Z80Reset", 0x0FC350: "Z80Load",
             0x0FC3E6: "Z80Reset2", 0x0FC438: "SndBoot2", 0x00867A: "VdpSetRegs", 0x0086F6: "VramClearAlt",
             0x00871A: "VramClear", 0x008162: "CramClear", 0x00818C: "VsramClear", 0x0081B6: "VdpSetScroll2",
             0x001BE8: "ReadPort2Id", 0x00884C: "InitPort1"},
    prefix="Boot",
)


class HwGen(pg.Gen):
    def __init__(self, spec):
        super().__init__(spec)
        self.bound = {}
        self.barriers = set()
        self._cur = 0

    # --- control flow: back edges and SR writes end a piece
    def kind(self, i):
        t, mn, args = self.parsed(i)
        if self.is_sr_write(mn, args):
            return "srw", []
        return super().kind(i)

    def spdelta(self, i):
        t, mn, args = self.parsed(i)
        t_ = t
        if mn == "lea":
            if args[1] != "%sp":
                return 0
            assert parse_op(args[0])[0] == "abs", t_      # Boot's `lea $FFFF9C,sp`: the initial stack pointer, so no change
            return 0
        m = re.fullmatch(r"(addq|subq|addi|subi|adda|suba)([wl])", mn)
        if m and args[1] == "%sp":                # addq/adda #n,sp
            v = parse_op(args[0])
            assert v[0] == "i", t_
            return (1 if m.group(1).startswith("add") else -1) * v[1]
        if "%sp" in " ".join(args) and not any(x in " ".join(args) for x in ("%sp@-", "%sp@+")) and mn[:4] not in ("movem", "lea") \
                and not (mn.startswith("move") and args[-1] == "%sp"):
            return 0                              # a read through sp (`%sp@`, `%sp@(n)`): the pointer does not move
        return super().spdelta(i)

    @staticmethod
    def is_sr_write(mn, args):
        return mn.startswith("move") and len(args) == 2 and args[1] == "%sr"

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
            total = 0
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
                total += max(pg.cc.insn_costs(i.words, self.T) or [0]) + 6
                if cls == "stop":
                    exits.add((a, d))
                    self.barriers.add(a)
                    t, mn, args = self.parsed(i)
                    if mn in pg.CALLS:
                        todo.append(nxt)
                    continue
                d2 = d + self.spdelta(i)
                if cls == "srw":
                    exits.add((nxt, d2))
                    todo.append(nxt)
                    continue
                if cls == "bra":
                    nexts = [tg[0]]
                elif cls in ("bcc", "dbf"):
                    nexts = [tg[0], nxt]
                else:
                    nexts = [nxt]
                for n in nexts:
                    if self.inrange(n) and n <= a:          # a back edge: one pass of the loop is one block
                        exits.add((n, d2))
                        todo.append(n)
                    else:
                        stack.append((n, d2))
            self.bound[e] = total + 60
        return entries, reached

    def goto(self, tgt):
        if self.inrange(tgt) and tgt <= self._cur:
            return "hle_jump(ctx, 0x%06X); return;" % tgt
        return super().goto(tgt)

    # --- instructions
    def emit(self, i, labels):
        self._cur = i.addr
        t, mn, args = self.parsed(i)
        a = i.addr
        nxt = a + self.length(i)
        cmt = "/* %06X %s */" % (a, t)
        lab = "L_%06X:\n" % a if a in labels else ""
        base = self.T[i.words[0]]
        op = i.words[0]

        def line(s):
            tail = ""
            return lab + "\t" + s + tail + "  " + cmt

        cls, tg = self.kind(i)
        if cls != "srw" and not (mn == "movew" and args[0] == "%sr") and not (mn == "movel" and args[1] == "%usp"):
            ex = self.emit_extra(i, t, mn, args, line)
            if ex is not None:
                return ex
        # ---- SR / USP
        if cls == "srw":
            src = parse_op(args[0]) if args[0] != "%sp@+" else ("post", 7)
            if src[0] == "i":
                val = "0x%04Xu" % (src[1] & 0xFFFF)
                return line("hle_insn(ctx, %d); ctx->sr = %s; hle_jump(ctx, 0x%06X); return;" % (base, val, nxt))
            if src[0] == "post" and src[1] == 7:
                return line("{ uint16_t v = pop16(ctx); hle_insn(ctx, %d); ctx->sr = v; hle_jump(ctx, 0x%06X); return; }" % (base, nxt))
            raise AssertionError("sr write " + t)
        if mn == "movew" and args[0] == "%sr":
            dst = parse_op(args[1])
            assert dst == ("pre", 7), t
            return line("hle_insn(ctx, %d); push16(ctx, ctx->sr);" % base)
        if mn == "movel" and args[1] == "%usp":
            return line("hle_insn(ctx, %d); hle_set_usp(ctx->a[%d]);" % (base, parse_op(args[0])[1]))
        # ---- roxl / roxr by an immediate count on a data register
        m = re.fullmatch(r"rox([lr])([bwl])", mn)
        if m:
            sz = SZ[m.group(2)]
            cnt, r = parse_op(args[0]), parse_op(args[1])
            assert cnt[0] == "i" and r[0] == "d", t
            bits = BITS[sz]
            mk = "0x%Xu" % MASK[sz]
            sg = "0x%Xu" % (1 << (bits - 1))
            body = []
            for _ in range(cnt[1]):
                if m.group(1) == "l":
                    body.append("{ unsigned x = (sr & CC_X) ? 1 : 0, c = (v & %s) ? 1 : 0; v = ((v << 1) | x) & %s; sr = c ? (sr | CC_X | CC_C) : (sr & ~(CC_X | CC_C)); }" % (sg, mk))
                else:
                    body.append("{ unsigned x = (sr & CC_X) ? 1 : 0, c = v & 1; v = (v >> 1) | (x ? %s : 0); sr = c ? (sr | CC_X | CC_C) : (sr & ~(CC_X | CC_C)); }" % sg)
            return line("{ unsigned sr = ctx->sr; uint32_t v = ctx->d[%d] & %s; %s %s cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C, (sr & (CC_X | CC_C)) | ((v & %s) ? CC_N : 0) | (v ? 0 : CC_Z)); } hle_insn(ctx, %d);" %
                        (r[1], mk, " ".join(body), wr_stmt(r, sz, "v", "d"), sg, self.cost(i)))
        m = re.fullmatch(r"ro([lr])([bwl])", mn)
        if m:
            sz = SZ[m.group(2)]
            cnt, r = parse_op(args[0]), parse_op(args[1])
            assert cnt[0] == "i" and r[0] == "d" and 1 <= cnt[1] <= 8, t
            bits = BITS[sz]
            mk = "0x%Xu" % MASK[sz]
            sg = "0x%Xu" % (1 << (bits - 1))
            one = ("{ c = (v & %s) ? 1 : 0; v = ((v << 1) | c) & %s; }" % (sg, mk)) if m.group(1) == "l" else \
                  ("{ c = v & 1; v = (v >> 1) | (c ? %s : 0); }" % sg)
            return line("{ uint32_t v = ctx->d[%d] & %s; unsigned c = 0; for (int k = 0; k < %d; k++) %s %s cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, (c ? CC_C : 0) | ((v & %s) ? CC_N : 0) | (v ? 0 : CC_Z)); } hle_insn(ctx, %d);" %
                        (r[1], mk, cnt[1], one, wr_stmt(r, sz, "v", "d"), sg, self.cost(i)))
        # ---- movem
        m = re.fullmatch(r"movem([wl])", mn)
        if m:
            sz = 2 if m.group(1) == "w" else 4
            mask = i.words[1]
            n = bin(mask).count("1")
            cost = self.cost(i)
            src = parse_op(args[0])
            assert src[0] in ("post", "ind", "disp"), t
            regs = [b for b in range(16) if mask & (1 << b)]
            ld = "(uint32_t)(int32_t)(int16_t)ld16" if sz == 2 else "ld32"
            body = "".join("%s = %s(eaS + %d); " % ("ctx->d[%d]" % r if r < 8 else "ctx->a[%d]" % (r - 8), ld, sz * k)
                           for k, r in enumerate(regs))
            post = ("ctx->a[%d] += %d; " % (src[1], sz * n)) if src[0] == "post" else ""
            end = "hle_movem_l(ctx, %d, %d);" % (cost, n) if sz == 4 else "hle_insn(ctx, %d);" % cost
            ea = "ctx->a[%d]" % src[1] if src[0] != "disp" else "ctx->a[%d] + (uint32_t)(int32_t)(%d)" % (src[1], src[2])
            return line("{ uint32_t eaS = %s; int io = !plain(eaS); if (io) io_pre(ctx, 0x%04X, 0x%06X); %s%sif (io) hle_post(ctx, %d); else %s }" %
                        (ea, op, nxt, body, post, cost, end))
        # ---- bit operations on memory
        if mn in ("btst", "bset", "bclr", "bchg") and len(args) == 2:
            d = parse_op(args[1])
            if is_mem(d):
                s = parse_op(args[0])
                pre, ea = ea_of(d, 1, "d")
                bitexpr = ("%d" % (s[1] & 7)) if s[0] == "i" else "(ctx->d[%d] & 7)" % s[1]
                act = {"btst": "", "bset": "st8(eaD, v | m);", "bclr": "st8(eaD, v & ~m);", "bchg": "st8(eaD, v ^ m);"}[mn]
                return line("{ %s int io = !plain(eaD); if (io) io_pre(ctx, 0x%04X, 0x%06X); unsigned b = %s; uint32_t m = 1u << b; uint8_t v = ld8(eaD); "
                            "cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z); %s if (io) hle_post(ctx, %d); else hle_insn(ctx, %d); }" %
                            (pre, op, nxt, bitexpr, act, base, base))
        if mn in ("st", "sf"):
            o = parse_op(args[0])
            if is_mem(o):
                pre, ea = ea_of(o, 1, "d")
                val = "0xFF" if mn == "st" else "0"
                return line("{ %s int io = !plain(eaD); if (io) io_pre(ctx, 0x%04X, 0x%06X); st8(eaD, %s); if (io) hle_post(ctx, %d); else hle_insn(ctx, %d); }" %
                            (pre, op, nxt, val, base, base))
        return super().emit(i, labels)


    def wrap(self, i, pre, names, body, cost, x=None):
        """one instruction with memory operands: the access happens between hle_pre and hle_post when any of them is I/O"""
        nxt = i.addr + self.length(i)
        if not names:
            return "{ %s%s } hle_insn(ctx, %s);" % (pre, body, cost)
        io = " || ".join("!plain(%s)" % n for n in names)
        return ("{ %sint io = %s; if (io) io_pre(ctx, 0x%04X, 0x%06X); %s if (io) hle_post(ctx, %s); else hle_insn(ctx, %s); }" %
                (pre, io, i.words[0], nxt, body, cost, cost))

    def emit_extra(self, i, t, mn, args, line):
        base = self.T[i.words[0]]
        # ---- mulu.w / muls.w <ea>,Dn: 38 + 2n + the ea time (n = ones / bit transitions of the source)
        m = re.fullmatch(r"mul([us])w", mn)
        if m:
            src, dst = parse_op(args[0]), parse_op(args[1])
            assert dst[0] == "d"
            pre, nm = (ea_of(src, 2, "s")[0], ["eaS"]) if is_mem(src) else ("", [])
            sg = m.group(1) == "s"
            if sg:
                calc = ("uint32_t res = (uint32_t)((int32_t)(int16_t)ctx->d[%d] * (int32_t)(int16_t)s); uint32_t tt = ((uint32_t)s << 1) & 0x1FFFF; "
                        "unsigned n = 0; for (int k = 0; k < 16; k++) if (((tt >> k) & 1) != ((tt >> (k + 1)) & 1)) n++;" % dst[1])
            else:
                calc = ("uint32_t res = (ctx->d[%d] & 0xFFFFu) * (uint32_t)s; unsigned n = 0; for (int k = 0; k < 16; k++) n += (s >> k) & 1;" % dst[1])
            body = ("uint32_t s = %s; %s ctx->d[%d] = res; cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, (res & 0x80000000u ? CC_N : 0) | (res ? 0 : CC_Z));" %
                    (rd_expr(src, 2, "s"), calc, dst[1]))
            return line("{ %s%s hle_insn(ctx, %d + 38 + 2 * n); }" % (pre, body, base))
        # ---- divu.w <ea>,Dn (GPGX UseDivuCycles)
        if mn == "divuw":
            src, dst = parse_op(args[0]), parse_op(args[1])
            assert dst[0] == "d"
            pre = ea_of(src, 2, "s")[0] if is_mem(src) else ""
            r = dst[1]
            return line("{ %suint32_t s = %s; if (!s) __builtin_trap(); uint32_t dd = ctx->d[%d]; uint32_t q = dd / s, rem = dd %% s; "
                        "if (q < 0x10000) { uint32_t d = dd, sv = s << 16; int cyc = 76; for (int k = 0; k < 15; k++) { if ((int32_t)d < 0) { d <<= 1; d -= sv; } "
                        "else { d <<= 1; cyc += 4; if (d >= sv) { d -= sv; cyc -= 2; } } } ctx->d[%d] = (q & 0xFFFFu) | (rem << 16); "
                        "cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, ((q & 0x8000) ? CC_N : 0) | (q ? 0 : CC_Z)); hle_insn_x(ctx, %d + cyc, cyc); } "
                        "else { cc_set(ctx, CC_N | CC_V | CC_C, CC_N | CC_V); hle_insn(ctx, %d + 10); } }" %
                        (pre, rd_expr(src, 2, "s"), r, r, base, base))
        # ---- abcd / sbcd -(Ay),-(Ax) (byte)
        if mn in ("abcd", "sbcd"):
            src, dst = parse_op(args[0]), parse_op(args[1])
            assert src[0] == "pre" and dst[0] == "pre" and 7 not in (src[1], dst[1]), t
            body = ("ctx->a[%d] -= 1; uint32_t sv = ld8(ctx->a[%d]); ctx->a[%d] -= 1; uint32_t eaD = ctx->a[%d]; uint32_t dv = ld8(eaD); "
                    "st8(eaD, bcd(ctx, %d, sv, dv));" % (src[1], src[1], dst[1], dst[1], 1 if mn == "sbcd" else 0))
            return line(self.wrap(i, "", ["ctx->a[%d]" % dst[1]], body, base))
        # ---- adda / suba
        m = re.fullmatch(r"(add|sub)a([wl])", mn)
        if m:
            src, dst = parse_op(args[0]), parse_op(args[1])
            assert dst[0] == "a"
            sz = SZ[m.group(2)]
            pre, nm = (ea_of(src, sz, "s")[0], ["eaS"]) if is_mem(src) else ("", [])
            v = rd_expr(src, sz, "s")
            if sz == 2:
                v = "(uint32_t)(int32_t)(int16_t)%s" % v
            body = "ctx->a[%d] %s= %s;" % (dst[1], "+" if m.group(1) == "add" else "-", v)
            return line(self.wrap(i, pre, nm, body, self.cost(i)))
        # ---- eor Dn,<ea>
        m = re.fullmatch(r"eor([bwl])", mn)
        if m:
            sz = SZ[m.group(1)]
            src, dst = parse_op(args[0]), parse_op(args[1])
            assert src[0] == "d"
            pre, nm = (ea_of(dst, sz, "d")[0], ["eaD"]) if is_mem(dst) else ("", [])
            body = "uint32_t v = (%s ^ %s) & 0x%Xu; %s %s" % (rd_expr(dst, sz, "d"), rd_expr(src, sz, "s"), MASK[sz], wr_stmt(dst, sz, "v", "d"), lg(sz, "v"))
            return line(self.wrap(i, pre, nm, body, self.cost(i)))
        # ---- andi / ori / eori #imm,CCR
        m = re.fullmatch(r"(and|or|eor)ib", mn)
        if m and args[1] == "%ccr":
            v = parse_op(args[0])[1] & 0x1F
            o = {"and": "&", "or": "|", "eor": "^"}[m.group(1)]
            return line("ctx->sr = (uint16_t)((ctx->sr & ~0x1Fu) | ((ctx->sr %s 0x%Xu) & 0x1Fu)); hle_insn(ctx, %d);" % (o, v, base))
        # ---- move to CCR
        if mn == "movew" and args[1] == "%ccr":
            src = parse_op(args[0])
            pre, nm = (ea_of(src, 2, "s")[0], ["eaS"]) if is_mem(src) else ("", [])
            return line(self.wrap(i, pre, nm, "ctx->sr = (uint16_t)((ctx->sr & ~0x1Fu) | (%s & 0x1Fu));" % rd_expr(src, 2, "s"), self.cost(i)))
        # ---- movem registers -> memory ((An), d(An), -(An) of an An other than sp)
        m = re.fullmatch(r"movem([wl])", mn)
        if m and re.fullmatch(r"%(?:[ad]\d|fp|sp)(?:-%(?:[ad]\d|fp|sp))?(?:/%(?:[ad]\d|fp|sp)(?:-%(?:[ad]\d|fp|sp))?)*", args[0]) \
:
            sz = 2 if m.group(1) == "w" else 4
            mask = i.words[1]
            n = bin(mask).count("1")
            dst = parse_op(args[1])
            assert dst[0] in ("ind", "disp", "pre", "post", "abs"), t
            regs = [b for b in range(16) if mask & (1 << b)]
            nxt = i.addr + self.length(i)
            cost = self.cost(i)
            end = "hle_movem_l(ctx, %d, %d);" % (cost, n) if sz == 4 else "hle_insn(ctx, %d);" % cost
            st = "st32" if sz == 4 else "st16"
            if dst[0] == "pre":
                # predecrement: the register mask is reversed (bit 0 = a7)
                regs = [15 - b for b in range(16) if mask & (1 << b)]
                body = "".join("ctx->a[%d] -= %d; %s(ctx->a[%d], %s); " % (dst[1], sz, st, dst[1], "ctx->d[%d]" % r if r < 8 else "ctx->a[%d]" % (r - 8)) for r in regs)
                return line("{ uint32_t eaD = ctx->a[%d] - %d; int io = !plain(eaD); if (io) io_pre(ctx, 0x%04X, 0x%06X); %sif (io) hle_post(ctx, %d); else %s }" %
                            (dst[1], sz, i.words[0], nxt, body, cost, end))
            pre, ea = ea_of(dst, sz, "d")
            body = "".join("%s(eaD + %d, %s); " % (st, sz * k, "ctx->d[%d]" % r if r < 8 else "ctx->a[%d]" % (r - 8)) for k, r in enumerate(regs))
            return line("{ %sint io = !plain(eaD); if (io) io_pre(ctx, 0x%04X, 0x%06X); %sif (io) hle_post(ctx, %d); else %s }" % (pre, i.words[0], nxt, body, cost, end))
        return None

    def gen_sized(self, i, kind, sz, args, base):
        s = super().gen_sized(i, kind, sz, args, base)
        ops = [parse_op(x) for x in args]
        if kind in ("clr", "tst", "not", "neg"):
            src, dst = None, ops[0]
        else:
            src, dst = ops[0], ops[1]
        pre, names = "", []
        if kind in pg.SHKIND:
            return s
        if src is not None and is_mem(src):
            pre += ea_of(src, sz, "s")[0]
            names.append("eaS")
        if is_mem(dst):
            pre += ea_of(dst, sz, "d")[0]
            names.append("eaD")
        if not names:
            return s
        m = re.fullmatch(r"\{ (.*) \} hle_insn\(ctx, (\d+)\);", s, re.S)
        assert m and m.group(1).startswith(pre), s
        body, n = m.group(1)[len(pre):], m.group(2)
        nxt = i.addr + self.length(i)
        io = " || ".join("!plain(%s)" % x for x in names)
        return ("{ %sint io = %s; if (io) io_pre(ctx, 0x%04X, 0x%06X); %s if (io) hle_post(ctx, %s); else hle_insn(ctx, %s); }" %
                (pre, io, i.words[0], nxt, body, n, n))


HEADER = r"""/* The cold-boot path and the hardware helpers it needs, as C blocks (t2-decomp#11, phase P5; docs/native.md "Cold boot").
 *
 * %(title)s
 *
 * Generated instruction by instruction from the disassembly by tools/py/bootgen.py (objdump with relocations and the
 * GPGX opcode cycle table; `costcheck.py` checks every charge).  The blocks are pieces as in piecegen.py (an entry, up
 * to the next jsr/bsr/jmp/rts, which the glue blocks of glue.c execute), with the instruction timeline of the sound
 * driver (game/sound.c): hle_insn for the instructions that touch work RAM and ROM only, hle_pre .. hle_post around the
 * ones that reach the I/O area, the Z80 or the VDP, so the access sees the clock the 68k core would give it.  A
 * backward branch leaves the piece (one pass of a loop is one block) and a write to SR ends it.  The VDP status word
 * has its bits 10-15 from the instruction prefetch (the word behind the reading instruction, as in game/vdpfill.c).
 */
#include "hle.h"
#include "gx.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#pragma GCC diagnostic ignored "-Wunused-label"

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

/* asl ('L') / lsl ('l') / lsr ('r') / asr ('a') / roxl ('x') by an immediate count (1..8) on a register */
static uint32_t shf(hle_ctx *c, char kind, uint32_t v, int n, int bits)
{
	uint32_t m = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1), sg = 1u << (bits - 1);
	uint32_t res = v & m;
	unsigned cb = 0, ov = 0, x = (c->sr & CC_X) ? 1 : 0;
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
	(void)x;
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

/* abcd (sub = 0) / sbcd (sub = 1) of one byte: GPGX's m68k_op_abcd_8_mm / sbcd_8_mm (undefined N, V included) */
static uint32_t __attribute__((unused)) bcd(hle_ctx *ctx, int sub, uint32_t src, uint32_t dst)
{
	uint32_t x = (ctx->sr & CC_X) ? 1 : 0, res, vraw, carry, bits = 0;
	if (!sub) {
		uint32_t lo = (src & 0x0F) + (dst & 0x0F) + x;
		uint32_t corf = (lo > 9) ? 6 : 0;
		uint32_t r2 = lo + (src & 0xF0) + (dst & 0xF0);
		vraw = ~r2;
		uint32_t r3 = r2 + corf;
		carry = (r3 > 0x9F) ? 1 : 0;
		if (carry)
			r3 -= 0xA0;
		res = r3 & 0xFF;
		vraw &= r3;
	} else {
		res = (dst & 0x0F) - (src & 0x0F) - x;
		uint32_t corf = (res > 0xF) ? 6 : 0;
		res += (dst & 0xF0) - (src & 0xF0);
		vraw = res;
		if (res > 0xFF) {
			res += 0xA0;
			carry = 1;
		} else {
			carry = res < corf;
		}
		res = (res - corf) & 0xFF;
		vraw &= ~res;
	}
	if (carry)
		bits |= CC_X | CC_C;
	if (res & 0x80)
		bits |= CC_N;
	if (vraw & 0x80)
		bits |= CC_V;
	cc_set(ctx, CC_X | CC_N | CC_V | CC_C, bits);
	if (res)
		cc_set(ctx, CC_Z, 0);
	return res;
}

/* work RAM and ROM are plain memory; anything else (Z80 area, I/O, VDP) is an I/O access made at the instruction's clock */
static inline int plain(uint32_t a)
{
	a &= 0xFFFFFFu;
	return a >= 0xE00000u || a < 0x400000u;
}
static uint32_t g_next_pc;   /* the word behind the instruction that makes the I/O access (the prefetch queue) */
static int g_own_sess;       /* this block started a VDP session (below) */
static void io_pre(hle_ctx *ctx, unsigned op, uint32_t next_pc)
{
	hle_pre(ctx);
	gx_m68k_set_ir(op);
	g_next_pc = next_pc;
}
/* The VDP is clocked by the CPU: its raster line follows the 68k clock, and a data port write stalls the CPU while the
 * FIFO is full.  Both need the "exact session" of game/vdpfill.c, which the first VDP access of a block opens (when no
 * C function above us has one) and block_end() closes: the line base and V counter are rewound to the session start
 * and the time spent is charged. */
static inline int is_vdp(uint32_t p) { return (p & 0xFFFFE0u) == 0xC00000u; }
static void vdp_enter(void)
{
	if (!g_own_sess && hle_nest_depth() == 0) {
		hle_dma_begin();
		hle_exact_session();
		g_own_sess = 1;
	}
	gx_vdp_follow_cpu_lines();
}
static inline void block_end(void)
{
	if (g_own_sess) {
		g_own_sess = 0;
		hle_vdp_end();
	}
}
static inline uint8_t ld8(uint32_t a)
{
	uint32_t p = a & 0xFFFFFFu;
	if (p >= 0xE00000u) return RAM8(p);
	if (p < 0x400000u) return ROM8(p);
	if (is_vdp(p)) vdp_enter();
	return bus_r8(p);
}
static inline uint16_t ld16(uint32_t a)
{
	uint32_t p = a & 0xFFFFFFu;
	if (p >= 0xE00000u) return RAM16(p);
	if (p < 0x400000u) return ROM16(p);
	if (is_vdp(p)) vdp_enter();
	if ((p & 0xFFFFE4u) == 0xC00004u)                    /* VDP status: bits 10-15 are the prefetched word */
		return (uint16_t)((bus_r16(p) & 0x3FF) | (ROM16(g_next_pc) & 0xFC00));
	return bus_r16(p);
}
static inline uint32_t ld32(uint32_t a)
{
	uint32_t p = a & 0xFFFFFFu;
	if (p >= 0xE00000u) return RAM32(p);
	if (p < 0x400000u) return ROM32(p);
	if (is_vdp(p)) vdp_enter();
	return bus_r32(p);
}
static inline void st8(uint32_t a, uint32_t v)
{
	uint32_t p = a & 0xFFFFFFu;
	if (p >= 0xE00000u) { RAM8(p) = (uint8_t)v; return; }
	if (is_vdp(p)) { vdp_enter(); hle_vdp_timed(1); bus_w8(p, (uint8_t)v); hle_vdp_timed(0); return; }
	bus_w8(p, (uint8_t)v);
}
static inline void st16(uint32_t a, uint32_t v)
{
	uint32_t p = a & 0xFFFFFFu;
	if (p >= 0xE00000u) { RAM16(p) = (uint16_t)v; return; }
	if (is_vdp(p)) { vdp_enter(); if (getenv("T2_BTRACE")) fprintf(stderr, "st16 %%06X <- %%04X clk %%d\n", p, v & 0xFFFF, gx_m68k_cycles()); hle_vdp_timed(1); bus_w16(p, (uint16_t)v); hle_vdp_timed(0); return; }
	bus_w16(p, (uint16_t)v);
}
static inline void st32(uint32_t a, uint32_t v)
{
	uint32_t p = a & 0xFFFFFFu;
	if (p >= 0xE00000u) { RAM32_W(p, v); return; }
	if (is_vdp(p)) { vdp_enter(); hle_vdp_timed(1); bus_w32(p, v); hle_vdp_timed(0); return; }
	bus_w32(p, v);
}

"""


def build(spec, runname="boot_run", registered=()):
    g = HwGen(spec)
    entries, reached = g.walk()
    labels = set(entries)
    for a in reached:
        cls, tg = g.kind(g.ins[a])
        for tt in tg:
            if g.inrange(tt) and tt > a:
                labels.add(tt)
    for e in list(entries):
        if g.kind(g.ins[e])[0] == "stop" or e in registered:   # a glue block / a block of another file sits there
            del entries[e]
    out = ["static void %s(hle_ctx *ctx, uint32_t pc)\n{\n\tswitch (pc) {" % runname]
    for e in sorted(entries):
        out.append("\tcase 0x%06X: goto L_%06X;" % (e, e))
    out.append("\tdefault: __builtin_trap();\n\t}")
    for a in sorted(reached):
        out.append(g.emit(g.ins[a], labels))
    out.append("}\n")
    for e in sorted(entries):
        nm = "bt_" + spec["entries"].get(e, "%s_%06X" % (spec["prefix"], e))
        ex = []
        for (xa, d) in sorted(entries[e]):
            ex.append("HLE_EXIT_SP(0x%06X, %d)" % (xa, d) if d else "0x%06X" % xa)
        out.append("HLE_RANGE(0x%06X, %s, %d, %s) { %s(ctx, 0x%06X); block_end(); }" % (e, nm, g.bound[e], ", ".join(ex), runname, e))
    return "\n".join(out), len(reached), g.barriers


def functions():
    out = {}
    for line in open(os.path.join(pg.cc.ROOT, "data/functions.txt")):
        if line.startswith("#") or not line.strip():
            continue
        main_, _, comment = line.partition("#")
        f = main_.split()
        rngs = [(int(f[0], 16), int(f[0], 16) + int(f[1]))]
        for m in re.finditer(r"\+([0-9a-f]+):(\d+)", comment):
            rngs.append((int(m.group(1), 16), int(m.group(1), 16) + int(m.group(2))))
        out[int(f[0], 16)] = (f[2], rngs)
    return out


# functions the native census lists that this generator must not touch: the DMA routines (another task) and the spin
# loops that wait for the VDP / an interrupt (docs/native.md "Waits")
AUTO_SKIP = {0x00800C, 0x011FA2, 0x01DC4E}


def auto():
    """bootgen.py auto ENTRIES.txt REGISTERED.txt OUTSITES.txt > port/game/auto2.c

    ENTRIES.txt: `ADDR COUNT` lines (tools/py/nativecensus.py: addresses the native runner reached from a C block with no
    block there), REGISTERED.txt: the addresses that already have a block (hle-list.txt of the census).  One pass per
    function of data/functions.txt: pieces from each of its entries."""
    ents = [int(x.split()[0], 16) for x in open(sys.argv[2]) if x.strip()]
    reg = set(int(x.split()[0], 16) for x in open(sys.argv[3]) if x.strip())
    import glob
    for pth in glob.glob(os.path.join(pg.cc.ROOT, "port/game/*.c")):    # also blocks that are off in the census (groups) or sit in other files
        if os.path.basename(pth) != "auto2.c":
            for m in re.finditer(r"\bHLE_(?:FUNC|RANGE|WAIT_FUNC|WAIT_RANGE)\w*\(\s*0x([0-9A-Fa-f]+)\s*,", open(pth).read()):
                reg.add(int(m.group(1), 16))
    funcs = functions()

    def func_of(a):
        for e, (n, rngs) in funcs.items():
            if any(lo <= a < hi for lo, hi in rngs):
                return e
        return None

    byf = {}
    for a in ents:
        if a in reg:
            continue
        byf.setdefault(func_of(a), []).append(a)
    texts, skipped, barriers, total = [], [], set(), 0
    for fe, alist in sorted(byf.items(), key=lambda kv: kv[0] or 0):
        if fe is None:
            skipped.append((alist[0], "(no function)", "address outside data/functions.txt"))
            continue
        if fe in AUTO_SKIP:
            skipped.append((fe, funcs[fe][0], "left to another task / a wait"))
            continue
        name, rngs = funcs[fe]
        spec = dict(ranges=rngs, entries={a: "Auto_%06X" % a for a in alist}, prefix="Auto")
        try:
            t, n, bar = build(spec, "boot_run_%06X" % fe, reg)
        except (AssertionError, KeyError, ValueError) as e:
            skipped.append((fe, name, "unsupported: %s" % str(e)[:70]))
            continue
        texts.append(t)
        total += n
        barriers |= bar
    title = "%d functions, %d instructions: the addresses the native runner reached from a C block with no block there\n * (docs/native.md \"The census\"); generated by `bootgen.py auto` from the native census" % (len(texts), total)
    sys.stdout.write(HEADER % dict(title=title))
    sys.stdout.write("\n".join(texts) + "\n")
    with open(sys.argv[4], "w") as f:
        for b in sorted(barriers):
            f.write("%06X\n" % b)
    for a, n, why in skipped:
        sys.stderr.write("skip $%06X %s: %s\n" % (a, n, why))
    sys.stderr.write("%d functions, %d instructions, %d skipped\n" % (len(texts), total, len(skipped)))


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "auto":
        return auto()
    text, n, barriers = build(SPEC)
    sys.stderr.write("glue sites: %s\n" % " ".join("%06X" % b for b in sorted(barriers)))
    title = "%d instructions: Reset, Boot, the Z80 loader, VdpSetRegs, the VRAM/CRAM/VSRAM clear helpers, the port-2 id read" % n
    sys.stdout.write(HEADER % dict(title=title))
    sys.stdout.write(text + "\n")
    sys.stderr.write("%d instructions\n" % n)


if __name__ == "__main__":
    main()
