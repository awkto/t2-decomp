#!/usr/bin/env python3
"""Static check of the per-instruction cycle charges in port/game/*.c.

Every original 68000 instruction a C replacement mirrors is charged with hle_insn(ctx, N)
(or HLE_SEQ, or a one-instruction helper that charges internally: moveq(), movew_m_d(), ...).
Neither difftest nor the lockstep gate sees a wrong N on a path the test states never run, so
this tool checks the numbers against the disassembly instead.

Truth source: src/t2.s is assembled (the incbin data is stubbed, only the code matters) and
disassembled for the address -> opcode map; the cost of an opcode is GPGX's own cycle table
(core/m68k/m68ki_cycles.h, indexed by the opcode word) plus the per-instruction extras that
m68kops.h adds (Bcc not taken, DBcc, Scc, movem per register, shift count, bit >= 16 on
bset/bclr/bchg Dn, mul/div ranges).  Each instruction therefore has a SET of valid charges
(for example `bne.s` is {10 taken, 8 not taken}).

Two ways a charge is tied to its original instruction:
  1. an address in the comment on the same line (`/* 0109A4 jsr %pc@(0x13240) */`, the
     generated files): checked directly against that instruction;
  2. otherwise the charges of a C function are aligned (dynamic programming, in order) with the
     instruction stream that starts at the function's anchor address (the HLE_FUNC/HLE_RANGE
     address, or the first $ADDR / sub_ADDR in the comment above the function).  A charge that is
     paired with an instruction whose cost set does not contain it is reported.

Findings are CANDIDATES: a mismatch can also be a checker limit (a branch emitted in a different
order than the assembler's, a data-dependent cost, a helper that charges elsewhere).  Read the
printed original instruction and judge.

usage: tools/py/costcheck.py [-v] [--all] [--summary] [FILE.c ...]     (default port/game/*.c)
       --all also shows the reviewed false positives listed in costcheck_known.txt;
       -v   also lists charges aligned to another mnemonic and unanchored functions;
       costcheck_anchors.txt gives the original address of helpers the tool cannot place itself.
env:   GPGX_DIR   directory with core/m68k/m68ki_cycles.h (default: ext/gpgx, /opt/t2/ext/gpgx,
                  build/port/gpgx-src, or the main checkout's ext/gpgx)
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


# ----------------------------------------------------------------------------- GPGX cycles

def find_cycles_h():
    cands = []
    if os.environ.get("GPGX_DIR"):
        cands.append(os.environ["GPGX_DIR"])
    cands += [os.path.join(ROOT, "ext", "gpgx"), "/opt/t2/ext/gpgx",
              os.path.join(ROOT, "build", "port", "gpgx-src")]
    try:  # a linked worktree: the main checkout holds ext/
        common = subprocess.check_output(["git", "-C", ROOT, "rev-parse", "--git-common-dir"],
                                         text=True, stderr=subprocess.DEVNULL).strip()
        main = os.path.dirname(os.path.abspath(os.path.join(ROOT, common)))
        cands += [os.path.join(main, "ext", "gpgx"), os.path.join(main, "build", "port", "gpgx-src")]
    except Exception:
        pass
    for c in cands:
        p = os.path.join(c, "core", "m68k", "m68ki_cycles.h")
        if os.path.exists(p):
            return p
    sys.exit("costcheck: m68ki_cycles.h not found (set GPGX_DIR)")


def load_table():
    t = open(find_cycles_h()).read()
    v = [int(x) for x in re.findall(r"(\d+)\*7", t)]
    if len(v) != 0x10000:
        sys.exit("costcheck: cycle table has %d entries" % len(v))
    return v


# ----------------------------------------------------------------------------- disassembly

class Insn:
    __slots__ = ("addr", "words", "text")

    def __init__(self, addr, words, text):
        self.addr, self.words, self.text = addr, words, text


def disassemble():
    """-> (list of Insn in address order, {addr: index})"""
    rom = os.path.join(ROOT, "build", "baserom.md")
    with tempfile.TemporaryDirectory() as td:
        inc = td
        if os.path.exists(rom):
            inc = os.path.join(ROOT, "build")
        else:
            with open(os.path.join(td, "baserom.md"), "wb") as f:
                f.truncate(4 << 20)       # the incbin data is not needed, only the sizes
        obj = os.path.join(td, "t2.o")
        subprocess.check_call(["m68k-linux-gnu-as", "-m68000", "--register-prefix-optional",
                               "-I", inc, "-o", obj, os.path.join(ROOT, "src", "t2.s")])
        out = subprocess.check_output(["m68k-linux-gnu-objdump", "-d", obj], text=True)
    insns = []
    rx = re.compile(r"^\s*([0-9a-f]+):\t((?:[0-9a-f]{4} )+)\s*(.*)$")
    for ln in out.splitlines():
        m = rx.match(ln)
        if not m:
            continue
        ws = [int(w, 16) for w in m.group(2).split()]
        if m.group(3).strip() == "" and insns:
            insns[-1].words += ws
        else:
            insns.append(Insn(int(m.group(1), 16), ws, m.group(3).strip()))
    return insns, {i.addr: n for n, i in enumerate(insns)}


# ----------------------------------------------------------------------------- cost model

def rng(lo, hi):
    return frozenset(range(lo, hi + 1, 2))


def insn_costs(words, T):
    """Set of cycle charges that are correct for this instruction on some path, or None when
    the cost is unknown (table entry 0 / unsupported)."""
    op = words[0]
    base = T[op]
    top = op >> 12
    if top == 6:                                   # bra / bsr / Bcc
        cond = (op >> 8) & 15
        if cond in (0, 1):
            return frozenset([base])
        if op & 0xFF == 0:
            return frozenset([base, base + 2])     # .w: taken 10, not taken 12
        return frozenset([base, base - 2])         # .s: taken 10, not taken 8
    if top == 5 and (op >> 6) & 3 == 3:
        mode = (op >> 3) & 7
        cond = (op >> 8) & 15
        if mode == 1:                              # DBcc: loops 10, cc true 12, expired 14
            if cond == 0:
                return frozenset([base])
            if cond == 1:
                return frozenset([base - 2, base + 2])
            return frozenset([base - 2, base, base + 2])
        return frozenset([base, base + 2]) if mode == 0 else frozenset([base])
    if (op & 0xFB80) == 0x4880 and (op & 0x38) != 0:   # movem (ext is not 'ext.w/l': mode 0)
        n = bin(words[1]).count("1") if len(words) > 1 else 0
        return frozenset([base + n * (8 if op & 0x40 else 4)])
    if top == 0xE:                                 # shifts / rotates
        if (op >> 6) & 3 == 3:
            return frozenset([base])               # memory form, count 1
        if op & 0x20:
            return frozenset(range(base, base + 128, 2))   # count in a register
        k = (op >> 9) & 7 or 8
        return frozenset([base + 2 * k])
    if (op & 0xF100) == 0x0100 and (op >> 3) & 7 != 1:     # bit op Dn,<ea>
        if (op >> 3) & 7 == 0 and (op >> 6) & 3 != 0:
            return frozenset([base, base + 2])
        return frozenset([base])
    if (op & 0xFF00) == 0x0800:                    # bit op #n,<ea>
        if (op >> 3) & 7 == 0 and (op >> 6) & 3 != 0 and len(words) > 1:
            return frozenset([base + (2 if (words[1] & 0x1F) >= 16 else 0)])
        return frozenset([base])
    if (op & 0xF1C0) in (0xC0C0, 0xC1C0):          # mulu / muls
        return frozenset(base + c for c in range(38, 71, 2))
    if (op & 0xF1C0) == 0x80C0:                    # divu
        return frozenset(base + c for c in range(70, 141, 2))
    if (op & 0xF1C0) == 0x81C0:                    # divs
        return frozenset(base + c for c in range(10, 170, 2))
    if base == 0:
        return None
    return frozenset([base])


# ----------------------------------------------------------------------------- C parsing

HEX_ADDR = re.compile(r"(?:\$|0x|sub_|loc_)([0-9A-Fa-f]{4,6})\b")
LINE_ADDR = re.compile(r"/\*\s*([0-9A-Fa-f]{5,6})\s+[a-z]")


def strip_line(line, in_cmt):
    """-> (code, comment_text, still_in_comment)"""
    code, cmt = [], []
    i, n = 0, len(line)
    while i < n:
        if in_cmt:
            j = line.find("*/", i)
            if j < 0:
                cmt.append(line[i:])
                i = n
            else:
                cmt.append(line[i:j])
                i = j + 2
                in_cmt = False
        else:
            if line.startswith("/*", i):
                in_cmt = True
                i += 2
            elif line.startswith("//", i):
                cmt.append(line[i + 2:])
                i = n
            elif line[i] == '"':
                j = i + 1
                while j < n and line[j] != '"':
                    j += 2 if line[j] == "\\" else 1
                code.append('""')
                i = j + 1
            elif line[i] == "'":
                j = i + 1
                while j < n and line[j] != "'":
                    j += 2 if line[j] == "\\" else 1
                code.append("0")
                i = j + 1
            else:
                code.append(line[i])
                i += 1
    return "".join(code), " ".join(cmt), in_cmt


def split_args(s):
    out, depth, cur = [], 0, []
    for ch in s:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if cur or out:
        out.append("".join(cur).strip())
    return out


def call_args(code, pos):
    """code[pos] == '(' -> (args list, end index)"""
    depth = 0
    for k in range(pos, len(code)):
        if code[k] == "(":
            depth += 1
        elif code[k] == ")":
            depth -= 1
            if depth == 0:
                return split_args(code[pos + 1:k]), k + 1
    return split_args(code[pos + 1:]), len(code)


class Env:
    def __init__(self, macros):
        self.macros = macros


def eval_set(expr, macros, params=None, depth=0):
    """Expression -> frozenset of ints (ternary alternatives), or None when not constant."""
    expr = expr.strip()
    if depth > 8 or not expr:
        return None
    # top-level ternary
    d = 0
    q = None
    for k, ch in enumerate(expr):
        if ch in "([":
            d += 1
        elif ch in ")]":
            d -= 1
        elif ch == "?" and d == 0 and q is None:
            q = k
    if q is not None:
        rest = expr[q + 1:]
        d = 0
        c = None
        for k, ch in enumerate(rest):
            if ch in "([":
                d += 1
            elif ch in ")]":
                d -= 1
            elif ch == ":" and d == 0:
                c = k
                break
        if c is None:
            return None
        a = eval_set(rest[:c], macros, params, depth + 1)
        b = eval_set(rest[c + 1:], macros, params, depth + 1)
        return None if a is None or b is None else a | b
    # parenthesised whole expression containing a ternary
    if expr.startswith("(") and expr.endswith(")"):
        inner = expr[1:-1]
        dd = 0
        ok = True
        for ch in inner:
            if ch == "(":
                dd += 1
            elif ch == ")":
                dd -= 1
                if dd < 0:
                    ok = False
                    break
        if ok and "?" in inner:
            return eval_set(inner, macros, params, depth + 1)
    expr = re.sub(r"\(\s*(?:int|unsigned|uint32_t|uint16_t|uint8_t)\s*\)", "", expr)
    unresolved = []

    def sub(m):
        name = m.group(0)
        if re.fullmatch(r"0[xX][0-9a-fA-F]+[uUlL]*|\d+[uUlL]*", name):
            return name.rstrip("uUlL")
        if params and name in params:
            v = params[name]
            if v is None or len(v) != 1:
                unresolved.append(name)
                return "0"
            return str(next(iter(v)))
        if name in macros:
            v = eval_set(macros[name], macros, None, depth + 1)
            if v is None or len(v) != 1:
                unresolved.append(name)
                return "0"
            return str(next(iter(v)))
        unresolved.append(name)
        return "0"

    e2 = re.sub(r"[A-Za-z_][A-Za-z_0-9]*|0[xX][0-9a-fA-F]+[uUlL]*|\d+[uUlL]*", sub, expr)
    if unresolved or not re.fullmatch(r"[0-9\s+\-*/()<>]*", e2):
        return None
    try:
        return frozenset([int(eval(e2))])      # noqa: S307 - sanitised arithmetic only
    except Exception:
        return None


class Charge:
    __slots__ = ("line", "vals", "addr", "cmt", "via", "mns", "ctext")

    def __init__(self, line, vals, addr, cmt, via):
        self.line, self.vals, self.addr, self.cmt, self.via = line, vals, addr, cmt, via
        self.mns = set()
        self.ctext = ""


class Func:
    def __init__(self, name, start, decl_cmt, header):
        self.name, self.start, self.decl_cmt, self.header = name, start, decl_cmt, header
        self.anchors = []
        self.lines = []          # (lineno, code, comment)
        self.charges = []
        self.is_hle = False
        self.macro_charges = 0
        self.anchor_src = None


def parse_file(path):
    src = open(path).read().split("\n")
    macros = {}
    for ln in src:
        m = re.match(r"\s*#\s*define\s+([A-Za-z_]\w*)\s+(.+?)\s*(?:/\*.*)?$", ln)
        if m and "(" not in m.group(1):
            macros[m.group(1)] = m.group(2)
    funcs = []
    in_cmt = False
    depth = 0
    pending = []         # (lineno, code, cmt) since the last depth-0 terminator
    cmt_block = []       # comment-only lines directly above
    cur = None
    for no, line in enumerate(src, 1):
        code, cmt, in_cmt2 = strip_line(line, in_cmt)
        was_cmt = in_cmt
        in_cmt = in_cmt2
        if depth == 0:
            if code.strip() == "" :
                if cmt.strip():
                    cmt_block.append(cmt)
                else:
                    cmt_block = [] if not was_cmt else cmt_block
                continue
            if code.lstrip().startswith("#"):
                cmt_block = []
                continue
            pending.append((no, code, cmt))
        # brace tracking
        for ch in code:
            if ch == "{":
                if depth == 0:
                    hdr = " ".join(p[1] for p in pending)
                    cur = Func(None, pending[0][0], " ".join(cmt_block) + " " + " ".join(p[2] for p in pending), hdr)
                    funcs.append(cur)
                    cur.lines.append((no, code[code.index("{") + 1:] if False else code, cmt))
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    pending = []
                    cmt_block = []
                    cur = None
        if depth > 0 and cur is not None and not (cur.lines and cur.lines[-1][0] == no):
            cur.lines.append((no, code, cmt))
        if depth == 0 and ";" in code and cur is None:
            pending = []
            cmt_block = []
    return src, macros, funcs


HLE_HDR = re.compile(r"HLE_(?:FUNC|RANGE)\w*\s*\(\s*(0[xX][0-9A-Fa-f]+)\s*,\s*(\w+)")
STATIC_HDR = re.compile(r"(\w+)\s*\(([^)]*)\)\s*\{?\s*$")


def load_anchor_overrides():
    out = {}
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "costcheck_anchors.txt")
    if os.path.exists(path):
        for ln in open(path):
            ln = ln.split("#")[0].split()
            if len(ln) == 3:
                out[(ln[0], ln[1])] = int(ln[2], 16)
    return out


ANCHOR_OVERRIDES = load_anchor_overrides()


def load_known():
    """Reviewed false positives: 'file function addr charged' (see costcheck_known.txt)."""
    out = set()
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "costcheck_known.txt")
    if os.path.exists(path):
        for ln in open(path):
            ln = ln.split("#")[0].split()
            if len(ln) == 4:
                out.add(tuple(ln))
    return out


KNOWN = load_known()


def finding_key(rel, fn, ins, c):
    return (os.path.basename(rel), fn, "%06X" % ins.addr, fmt_set(c.vals))


def analyse_header(f):
    m = HLE_HDR.search(f.header)
    if m:
        f.is_hle = True
        f.name = m.group(2)
        f.anchors.append(int(m.group(1), 16))
        f.anchor_src = "hle"
        return True
    h = f.header.split("{")[0]
    m = re.search(r"(\w+)\s*\(", h)
    if not m:
        return False
    f.name = m.group(1)
    args, _ = call_args(h, m.end() - 1)
    f.params = []
    for p in args:
        fp = re.search(r"\(\s*\*\s*(\w+)\s*\)", p)
        if fp:
            f.params.append(fp.group(1))
        elif p.strip() and p.strip() != "void":
            ids = re.findall(r"[A-Za-z_]\w*", p)
            f.params.append(ids[-1] if ids else "")
    for srcname, src in (("name", f.name), ("comment", f.decl_cmt)):
        for a in HEX_ADDR.finditer(src):
            f.anchors.append(int(a.group(1), 16))
        if f.anchors:
            f.anchor_src = srcname
            break                      # the name beats the comment; the first address wins
    del f.anchors[1:]
    return True


CHARGERS = ("hle_insn", "HLE_SEQ", "hle_post", "hle_movem_l", "hle_movem_w")


def scan_calls(f):
    """Ordered charge-relevant items of a function body: ('d', lineno, expr) for a direct charge,
    ('s', lineno, [expr..]) for HLE_SEQ, ('c', lineno, name, [argexpr..]) for any other call."""
    items = []
    for no, code, cmt in f.lines:
        for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", code):
            name = m.group(1)
            args, _ = call_args(code, m.end() - 1)
            if name in ("hle_insn", "hle_post", "hle_movem_l", "hle_movem_w"):
                if len(args) >= 2:
                    items.append(("d", no, args[1], name))
            elif name == "HLE_SEQ":
                items.append(("s", no, args[1:], name))
            else:
                items.append(("c", no, name, args))
    return items


class Expander:
    def __init__(self, funcs, macros):
        self.macros = macros
        self.fn = {f.name: f for f in funcs if f.name}
        self.items = {n: scan_calls(f) for n, f in self.fn.items()}
        for f in funcs:
            f.items = scan_calls(f)
        self.helpers = {}
        for n, f in self.fn.items():
            if f.is_hle or f.anchors or "static" not in f.header:
                continue
            if any(it[0] == "s" for it in self.items[n]):
                continue
            try:
                cnt = len(self.expand(n, [None] * len(getattr(f, "params", [])), 0, set()))
            except RecursionError:
                continue
            if 1 <= cnt <= 3:
                self.helpers[n] = f

    def expand(self, name, argsets, depth, stack):
        """-> list of (vals, via) for one call of helper `name`."""
        f = self.fn[name]
        env = dict(zip(getattr(f, "params", []), argsets))
        out = []
        if name in stack or depth > 6:
            return out
        for it in self.items[name]:
            if it[0] == "d":
                out.append((eval_set(it[2], self.macros, env), name))
            elif it[0] == "c":
                cn = it[2]
                if cn in self.fn and cn != name and (cn in self.helpers or not self.helpers_ready):
                    cf = self.fn[cn]
                    if cf.is_hle or cf.anchors or "static" not in cf.header:
                        continue
                    sub = [eval_set(a, self.macros, env) for a in it[3]]
                    out.extend(self.expand(cn, sub, depth + 1, stack | {name}))
        return out

    helpers_ready = False

    def events(self, f):
        """Ordered Charge list of f with helper calls expanded."""
        out = []
        lines = f.lines
        for it in f.items:
            no = it[1]
            li = next(i for i, l in enumerate(lines) if l[0] == no)
            cmt = lines[li][2]
            cm = comment_mns(cmt) if cmt.strip() else set()
            ctext = cmt if cmt.strip() else ""
            if not cm and li > 0:
                pcode, pcmt = lines[li - 1][1], lines[li - 1][2]
                if pcmt.strip() and not re.search(r"\b(hle_insn|HLE_SEQ)\b", pcode):
                    cm = comment_mns(pcmt)
            am = LINE_ADDR.search("/*" + cmt + "*/") if cmt else None
            addr = int(am.group(1), 16) if am else None
            if it[0] == "d":
                ch = Charge(no, eval_set(it[2], self.macros), addr, cmt, it[3])
                ch.mns = set(cm)
                ch.ctext = ctext
                out.append(ch)
            elif it[0] == "s":
                ml = comment_mn_list(cmt) if cmt.strip() else []
                for ai, a in enumerate(it[2]):
                    ch = Charge(no, eval_set(a, self.macros), addr, cmt, "HLE_SEQ")
                    ch.mns = set(cm)
                    if len(ml) == len(it[2]) and ml[ai]:
                        ch.mns = {ml[ai]}
                    ch.ctext = ctext if len(it[2]) == 1 else ""
                    out.append(ch)
            else:
                cn = it[2]
                if cn in self.helpers and cn != f.name:
                    hf = self.helpers[cn]
                    sub = [eval_set(a, self.macros, None) for a in it[3]]
                    for vals, _ in self.expand(cn, sub, 0, {f.name}):
                        ch = Charge(no, vals, addr, cmt, cn)
                        ch.mns = helper_mns(cn) or set(cm)
                        ch.ctext = ctext if len(self.expand(cn, [None] * len(it[3]), 0, set())) == 1 else ""
                        out.append(ch)
        return out


# ----------------------------------------------------------------------------- mnemonics

BASES = {"move", "movea", "moveq", "add", "adda", "addi", "addq", "addx", "sub", "suba", "subi", "subq",
         "subx", "cmp", "cmpa", "cmpi", "cmpm", "and", "andi", "or", "ori", "eor", "eori", "not", "neg",
         "negx", "clr", "tst", "lea", "pea", "jsr", "jmp", "bsr", "bra", "btst", "bset", "bclr", "bchg",
         "lsl", "lsr", "asl", "asr", "rol", "ror", "roxl", "roxr", "swap", "ext", "exg", "link", "unlk",
         "rts", "rte", "rtr", "nop", "movem", "mulu", "muls", "divu", "divs", "abcd", "sbcd", "nbcd",
         "tas", "chk", "trap", "movep", "bcc", "dbcc", "scc"}
FAMILY = {"movea": "move", "adda": "add", "addi": "add", "addq": "add", "addx": "add", "suba": "sub",
          "subi": "sub", "subq": "sub", "subx": "sub", "cmpa": "cmp", "cmpi": "cmp", "cmpm": "cmp",
          "andi": "and", "ori": "or", "eori": "eor", "asl": "shift", "asr": "shift", "lsl": "shift",
          "lsr": "shift", "rol": "shift", "ror": "shift", "roxl": "shift", "roxr": "shift", "bra": "jmp",
          "jmp": "jmp", "moveq": "moveq", "bsr": "jsr", "jsr": "jsr"}
CCS = {"eq", "ne", "cc", "cs", "hi", "ls", "pl", "mi", "ge", "lt", "gt", "le", "hs", "lo", "vc", "vs"}


def norm_mn(tok):
    """Mnemonic token (objdump AT&T, GNU Motorola or a comment's) -> canonical class, or None."""
    t = tok.lower().strip(".,:;/()")
    t = re.sub(r"\.(b|w|s|l)$", "", t)
    for cand in (t, t[:-1] if len(t) > 2 and t[-1] in "bwls" else None):
        if not cand:
            continue
        if cand in ("dbf", "dbra", "dbt") or (cand.startswith("db") and cand[2:] in CCS):
            return "dbcc"
        if cand in BASES:
            return FAMILY.get(cand, cand)
        if cand == "bcc":
            return "bcc"
        if cand.startswith("b") and cand[1:] in CCS:
            return "bcc"
        if cand.startswith("s") and cand[1:] in CCS | {"f", "t"}:
            return "scc"
    return None


def comment_mn_list(cmt):
    """Ordered mnemonic classes of an 'a / b / c' comment (None entries kept)."""
    out = []
    for part in re.split(r"[/;]| - ", cmt):
        toks = re.findall(r"[A-Za-z][A-Za-z0-9_.]*", part)
        m = None
        for t in toks[:2]:
            m = norm_mn(t)
            if m:
                break
        if toks:
            out.append(m)
    return out


def comment_mns(cmt):
    """Mnemonic classes named in a comment ('lsr.w #3,d0', 'bne.s taken / move.w ...')."""
    out = set()
    for part in re.split(r"[/;,+]| - |\bthen\b", cmt):
        toks = re.findall(r"[A-Za-z][A-Za-z0-9_.]*", part)
        for t in toks[:2]:
            m = norm_mn(t)
            if m:
                out.add(m)
                break
    return out


HELPER_MN = re.compile(r"^([a-z]+?)\d?[wbl]?(?:_.*)?$")


def helper_mns(name):
    m = HELPER_MN.match(name)
    if m:
        c = norm_mn(m.group(1))
        if c:
            return {c}
    if "exit" in name:
        return {"jmp"}
    if name in ("bs", "bw"):
        return {"bcc"}
    if name.startswith("jsr"):
        return {"jsr"}
    return set()


# ----------------------------------------------------------------------------- comment -> opcode

_asm_cache = {}
BRANCHY = re.compile(r"^(b[a-z]{2}|bra|bsr|db[a-z]{1,2}|jsr|jmp)(\.[sw])?$")


def comment_insn(cmt):
    """A comment that is exactly one instruction ('lsr.w #3,d0', 'move.w d0,(0x24,a6)') -> its
    machine words (assembled with GNU as), else None."""
    t = cmt.strip().strip("*").strip()
    t = re.split(r"\s+(?:-|--|;|:|taken|not taken|\(|=)\s*", t)[0].strip() if t else t
    t = re.sub(r"\s+(taken|not taken|falls through|expires|continues)\s*$", "", t)
    m = re.match(r"^([A-Za-z]+(?:\.[bwslBWSL])?)\s*(.*)$", t)
    if not m or " / " in t or "," in m.group(1):
        return None
    mn, ops = m.group(1).lower(), m.group(2).strip()
    if norm_mn(mn.split(".")[0]) is None and not BRANCHY.match(mn):
        return None
    if mn in ("bcc.s", "bcc.w", "bcc"):
        return None
    ops = re.sub(r"\$([0-9A-Fa-f]+)", r"0x\1", ops)
    ops = ops.replace("(abs).l", "(0x10000).l")
    if re.search(r"\(d16|d8|\bXn|\bAn\b|\bDn\b|<ea>|\bimm\b|\.\.\.|\bx\d", ops):
        return None
    if BRANCHY.match(mn):
        ops = re.sub(r"(?<![\w.])[A-Za-z_]\w*(?=\s*(?:\)|$|,\s*pc))", "", ops) if False else ops
        if mn.startswith("db"):
            ops = re.sub(r",\s*[A-Za-z_][\w+]*\s*$", ",.+6", ops) if not re.search(r"[0-9]\s*$", ops) else ops
        elif mn in ("jsr", "jmp"):
            if re.search(r"\)\.l|\.l\)?$", ops.replace(" ", "")):
                ops = "(0x10000).l"
            elif re.match(r"^\(?[A-Za-z_][\w+]*(,\s*pc)?\)?$", ops):
                ops = "(0x10,pc)"
        elif re.match(r"^[A-Za-z_]\w*$", ops):
            ops = ".+6"
    ops = re.sub(r"\(([A-Za-z_]\w*),pc\)", "(0x10,pc)", ops)
    src = mn + (" " + ops if ops else "")
    if src in _asm_cache:
        return _asm_cache[src]
    words = None
    with tempfile.TemporaryDirectory() as td:
        sp = os.path.join(td, "a.s")
        with open(sp, "w") as f:
            f.write("\t.text\n\t" + src + "\n")
        r = subprocess.run(["m68k-linux-gnu-as", "-m68000", "--register-prefix-optional", "-o",
                            os.path.join(td, "a.o"), sp], capture_output=True, text=True)
        if r.returncode == 0:
            out = subprocess.check_output(["m68k-linux-gnu-objdump", "-d", os.path.join(td, "a.o")], text=True)
            ws = []
            for ln in out.splitlines():
                mm = re.match(r"^\s*[0-9a-f]+:\t((?:[0-9a-f]{4} )+)", ln)
                if mm:
                    ws += [int(w, 16) for w in mm.group(1).split()]
            words = ws or None
    _asm_cache[src] = words
    return words

# ----------------------------------------------------------------------------- checking

def fmt_set(s):
    if s is None:
        return "?"
    s = sorted(s)
    if len(s) > 6:
        return "%d..%d" % (s[0], s[-1])
    return "/".join(str(x) for x in s)


def align(charges, window, T):
    """DP: charges vs window [(Insn, optset, mnemonic)] -> (score, [(charge_idx, win_idx)])."""
    n, m = len(charges), len(window)
    NEG = -1e9
    CSKIP, ASKIP = -1.0, -0.5
    dp = [[NEG] * (m + 1) for _ in range(n + 1)]
    bk = [[None] * (m + 1) for _ in range(n + 1)]
    dp[0][0] = 0.0
    for i in range(n + 1):
        for j in range(m + 1):
            cur = dp[i][j]
            if cur == NEG:
                continue
            if j < m:
                v = cur + ASKIP
                if v > dp[i][j + 1]:
                    dp[i][j + 1] = v
                    bk[i][j + 1] = (i, j, 0)
            if i < n:
                v = cur + CSKIP
                if v > dp[i + 1][j]:
                    dp[i + 1][j] = v
                    bk[i + 1][j] = (i, j, 1)
            if i < n and j < m:
                ok, mnok = pair_state(charges[i], window[j])
                if mnok is True:
                    sc = 5.0 if ok else 2.0
                elif mnok is None:
                    sc = 3.0 if ok else 0.5
                else:
                    sc = 1.0 if ok else 0.0
                v = cur + sc
                if v > dp[i + 1][j + 1]:
                    dp[i + 1][j + 1] = v
                    bk[i + 1][j + 1] = (i, j, 2)
    j = max(range(m + 1), key=lambda jj: dp[n][jj])
    best = dp[n][j]
    i = n
    pairs = []
    while (i, j) != (0, 0):
        pi, pj, kind = bk[i][j]
        if kind == 2:
            pairs.append((pi, pj))
        i, j = pi, pj
    return best, pairs[::-1]


def pair_state(c, w):
    """-> (cost compatible, mnemonic agrees: True/False/None=no evidence)"""
    cv, ov = c.vals, w[1]
    ok = cv is None or ov is None or bool(cv & ov)
    mnok = None if not c.mns else (w[2] in c.mns)
    return ok, mnok


MIN_AVG = 2.4          # a comment-derived anchor must align at least this well per charge
SEED_AVG = 3.2         # ... and a searched one this well


def seed_search(rest, insns, opts, mkwin, T):
    """No usable anchor: find where in the ROM code the charge sequence fits best."""
    n = len(rest)
    if n < 4:
        return None
    first = rest[:8]
    cands = []
    for k in range(len(insns)):
        # greedy prefix match, gaps up to 2 asm instructions
        j, hit = k, 0
        for c in first:
            for g in range(3):
                if j + g >= len(insns):
                    break
                o = opts(j + g)
                mn = norm_mn(insns[j + g].text.split()[0]) if insns[j + g].text else None
                ok = c.vals is None or o is None or bool(c.vals & o)
                if ok and (not c.mns or mn in c.mns):
                    hit += 1
                    j = j + g + 1
                    break
            else:
                j += 1
        if hit >= min(len(first), 6):
            cands.append((hit, k))
    cands.sort(reverse=True)
    best = None
    for _, k in cands[:12]:
        window = mkwin(k)
        sc, pairs = align(rest, window, T)
        if best is None or sc > best[0]:
            best = (sc, pairs, window)
    if best is not None and os.environ.get("COSTCHECK_DEBUG"):
        print("   seed best %.2f/charge at %06X" % (best[0] / n, best[2][0][0].addr))
    if best is None or best[0] / n < SEED_AVG:
        return None
    return best


def check_file(path, insns, index, T, opts_cache, verbose):
    src, macros, funcs = parse_file(path)
    for f in funcs:
        analyse_header(f)
    funcs = [f for f in funcs if f.name]
    ex = Expander(funcs, macros)
    ex.helpers_ready = True
    helpers = ex.helpers
    rel = os.path.relpath(path, ROOT)
    stats = dict(charges=0, direct=0, aligned=0, unchecked=0, bad=0)
    findings = []

    def opts(k):
        if k not in opts_cache:
            opts_cache[k] = insn_costs(insns[k].words, T)
        return opts_cache[k]

    for f in funcs:
        if f.name in helpers:
            continue
        evs = ex.events(f)
        stats["charges"] += len(evs)
        rest = []
        for c in evs:
            if c.addr is None and c.ctext.strip() and c.vals is not None:
                w = comment_insn(c.ctext)
                if w:
                    o = insn_costs(w, T)
                    stats["cmt"] = stats.get("cmt", 0) + 1
                    if o is not None and not (c.vals & o):
                        findings.append((c.line, f.name, c, Insn(0, w, "(comment) " + c.ctext.strip()), o, "cmt"))
                        stats["bad"] += 1
            if c.addr is not None and c.addr in index:
                stats["direct"] += 1
                o = opts(index[c.addr])
                if c.vals is not None and o is not None and not (c.vals & o):
                    findings.append((c.line, f.name, c, insns[index[c.addr]], o, "addr"))
                    stats["bad"] += 1
            else:
                rest.append(c)
        if not rest:
            continue
        def mkwin(k0):
            return [(insns[k], opts(k), norm_mn(insns[k].text.split()[0]) if insns[k].text else None)
                    for k in range(k0, min(k0 + wl, len(insns)))]

        wl = max(3 * len(rest) + 40, 120)
        best = None
        reliable = f.is_hle or (f.anchors and f.anchor_src == "name")
        cands = [a for a in f.anchors if a in index]
        for anchor in cands:
            window = mkwin(index[anchor])
            sc, pairs = align(rest, window, T)
            best = (sc, pairs, window)
        if best is None or (not reliable and best[0] / len(rest) < MIN_AVG):
            best = seed_search(rest, insns, opts, mkwin, T)
        if best is None:
            stats["unchecked"] += len(rest)
            if verbose:
                print("  [no anchor] %s:%d %s (%d charges)" % (rel, f.start, f.name, len(rest)))
            continue
        _, pairs, window = best
        paired = {ci for ci, _ in pairs}
        for ci, wj in pairs:
            c = rest[ci]
            ok, mnok = pair_state(c, window[wj])
            stats["aligned"] += 1
            if not ok and mnok is False:
                stats["silent"] = stats.get("silent", 0) + 1
                if verbose:
                    findings.append((c.line, f.name, c, window[wj][0], window[wj][1], "diff-mn"))
            if not ok and mnok is not False:
                tier = "HIGH" if mnok else "weak"
                # the same charge fits a neighbouring instruction: probably a branch path the C
                # repeats (else-arm) or a different order, not a wrong number
                for k in range(max(0, wj - 14), min(len(window), wj + 15)):
                    if k != wj:
                        ok2, mn2 = pair_state(c, window[k])
                        if ok2 and mn2 is not False:
                            tier = "alt"
                            break
                findings.append((c.line, f.name, c, window[wj][0], window[wj][1], tier))
                stats["bad"] += 1
        stats["unchecked"] += len(rest) - len(paired)
    return rel, stats, sorted(findings, key=lambda x: x[0])


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    verbose = "-v" in sys.argv
    summary = "--summary" in sys.argv
    files = args or sorted(os.path.join(ROOT, "port", "game", f)
                           for f in os.listdir(os.path.join(ROOT, "port", "game")) if f.endswith(".c"))
    T = load_table()
    insns, index = disassemble()
    cache = {}
    tot = 0
    print("%-24s %8s %7s %8s %9s %5s" % ("file", "charges", "direct", "aligned", "unchecked", "bad"))
    allf = []
    show_all = "--all" in sys.argv
    for p in files:
        rel, st, fnd = check_file(os.path.abspath(p), insns, index, T, cache, verbose)
        if not show_all:
            fnd = [x for x in fnd if finding_key(rel, x[1], x[3], x[2]) not in KNOWN]
        print("%-24s %8d %7d %8d %9d %5d" % (os.path.basename(rel), st["charges"], st["direct"],
                                              st["aligned"], st["unchecked"], len(fnd)))
        if verbose:
            print("    (comment-checked: %d; silent = aligned to another mnemonic and cost differs: %d)" % (
                st.get("cmt", 0), st.get("silent", 0)))
        tot += sum(1 for x in fnd if x[5] != "alt")
        allf.append((rel, fnd))
    nalt = sum(1 for _, fnd in allf for x in fnd if x[5] == "alt")
    print("candidate mismatches: %d, plus %d tagged [alt] (the charge also fits a nearby original "
          "instruction: often a repeated branch arm, but see EndoHitDown)" % (tot, nalt))
    if summary:
        return
    for rel, fnd in allf:
        for line, fn, c, ins, o, tier in fnd:
            print("%s:%d [%s] %s: charged %s, original %06X %s (%s) expects %s" % (
                rel, line, tier, fn, fmt_set(c.vals), ins.addr, ins.text, c.via, fmt_set(o)))
            if c.cmt.strip():
                print("      comment: %s" % c.cmt.strip()[:100])


if __name__ == "__main__":
    main()
