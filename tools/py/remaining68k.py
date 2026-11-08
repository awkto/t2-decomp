#!/usr/bin/env python3
"""docs/remaining68k.md generator (t2-decomp#11, phase P5): the 68k instructions that still run on the
emulated CPU when every C replacement is on (--hle all), measured over the gate routes, grouped by why.

    port/tools/ipprof.sh                       # on a VM: runs the routes, writes runs/ipprof/*.txt
    python3 -I tools/py/remaining68k.py [runs/ipprof] > docs/remaining68k.md

Inputs: the profiles written by port/src/ipprof.c (T2_IPPROF), runs/ipprof/hle-list.txt (`t2port --hle all
--hle-list`), data/functions.txt and the ROM (rom/baserom.md, to classify the instruction at each address).

Profile lines: T/N ADDR COUNT (interpreted top level / nested in a C function), E ADDR ENTERED DECLINED BAILED
(a replaced entry the CPU reached, and how often the C layer handed it back to the 68k), C CALLER ADDR COUNT.
"""
import bisect
import glob
import os
import re
import sys
from collections import defaultdict

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def parse_functions():
    funcs = []
    for line in open(os.path.join(ROOT, "data/functions.txt")):
        if line.startswith("#") or not line.strip():
            continue
        main, _, comment = line.partition("#")
        f = main.split()
        entry, size, name = int(f[0], 16), int(f[1]), f[2]
        ranges = [(entry, entry + size)]
        for p in re.finditer(r"\+([0-9a-f]+):(\d+)", comment):
            ranges.append((int(p.group(1), 16), int(p.group(1), 16) + int(p.group(2))))
        funcs.append(dict(entry=entry, name=name, ranges=ranges))
    return funcs


def build_index(funcs):
    idx = sorted((a, b, i) for i, f in enumerate(funcs) for a, b in f["ranges"])
    starts = [x[0] for x in idx]

    def find(addr):
        k = bisect.bisect_right(starts, addr) - 1
        if k >= 0 and idx[k][0] <= addr < idx[k][1]:
            return idx[k][2]
        return -1
    return find


def parse_hle_list(path):
    """`Name ($0012AB), Name2 ($...)` -> {addr: name}"""
    d = {}
    if os.path.exists(path):
        for line in open(path):
            for m in re.finditer(r"(\S+) \(\$([0-9A-Fa-f]{6})\)", line):
                d[int(m.group(2), 16)] = m.group(1)
    return d


def parse_profile(path):
    top, nest = defaultdict(int), defaultdict(int)
    ent = defaultdict(lambda: [0, 0, 0])
    hle = None
    frames = 0
    with open(path) as fh:
        if not fh.readline().startswith("# ipprof"):
            return None
        for line in fh:
            f = line.split()
            if f[0] == "hle":
                hle = f[1]
            elif f[0] == "frames":
                frames = int(f[1])
            elif f[0] == "T":
                top[int(f[1], 16)] += int(f[2])
            elif f[0] == "N":
                nest[int(f[1], 16)] += int(f[2])
            elif f[0] == "E":
                e = ent[int(f[1], 16)]
                for k in range(3):
                    e[k] += int(f[2 + k])
    return dict(hle=hle, frames=frames, top=top, nest=nest, ent=ent)


def route_of(path):
    b = os.path.basename(path)
    m = re.match(r"(st|rt)_(.+)\.\d+\.txt$", b)
    if not m:
        return "other", b
    return ("state" if m.group(1) == "st" else "route"), m.group(2)


def classify_insn(rom, a):
    w = (rom[a] << 8) | rom[a + 1]
    if w == 0x4E75: return "rts"
    if w == 0x4E73: return "rte"
    if w == 0x4E77: return "rtr"
    if w == 0x4E72: return "stop"
    if (w & 0xFFC0) == 0x4E80: return "jsr"
    if (w & 0xFFC0) == 0x4EC0: return "jmp"
    if (w & 0xFF00) == 0x6100: return "bsr"
    if (w & 0xFF00) == 0x6000: return "bra"
    if (w & 0xF000) == 0x6000: return "bcc"
    if (w & 0xF0F8) == 0x50C8: return "dbcc"
    if (w & 0xFFC0) == 0x46C0 or w in (0x027C, 0x007C, 0x0A7C): return "sr-write"
    if (w & 0xFFF0) == 0x4E40: return "trap"
    if (w & 0xFB80) == 0x4880: return "movem"
    return "insn"


BARRIER = ("jsr", "bsr", "jmp", "rts", "rte", "rtr", "stop", "trap")


def native_only():
    """addresses of the blocks that exist only in the native runner (group "native", port/game/*.c)"""
    got = set()
    for p in glob.glob(os.path.join(ROOT, "port/game/*.c")):
        for m in re.finditer(r'HLE_\w+_GROUP\(\s*0x([0-9A-Fa-f]+)\s*,[^,]*,[^,]*,\s*"native"', open(p).read()):
            got.add(int(m.group(1), 16))
        for m in re.finditer(r'native covers \$([0-9A-Fa-f]{6})-\$([0-9A-Fa-f]{6})', open(p).read()):
            got.update(range(int(m.group(1), 16), int(m.group(2), 16) + 1, 2))
    return got


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "runs/ipprof")
    funcs = parse_functions()
    find = build_index(funcs)
    hle = parse_hle_list(os.path.join(d, "hle-list.txt"))
    rom = open(os.path.join(ROOT, "rom/baserom.md"), "rb").read()

    runs = []
    for p in sorted(glob.glob(os.path.join(d, "*.txt"))):
        if os.path.basename(p) in ("hle-list.txt", "jobs.txt"):
            continue
        pr = parse_profile(p)
        if pr and pr["hle"] == "all":
            pr["kind"], pr["name"] = route_of(p)
            runs.append(pr)
    if not runs:
        sys.exit("no --hle all profiles in %s" % d)

    # per-function: does it own any C entry?  (HLE entries inside any of its ranges)
    func_c = defaultdict(list)
    for a, n in hle.items():
        i = find(a)
        if i >= 0:
            func_c[i].append((a, n))

    top, nest = defaultdict(int), defaultdict(int)
    ent = defaultdict(lambda: [0, 0, 0])
    seen_in = defaultdict(set)
    by_kind = defaultdict(lambda: dict(runs=0, frames=0, top=0, nest=0))
    per_run = []
    for r in runs:
        k = by_kind[r["kind"]]
        k["runs"] += 1
        k["frames"] += r["frames"]
        k["top"] += sum(r["top"].values())
        k["nest"] += sum(r["nest"].values())
        per_run.append((r["kind"], r["name"], r["frames"], sum(r["top"].values()), sum(r["nest"].values()),
                        len(set(r["top"]) | set(r["nest"]))))
        for a, c in r["top"].items():
            top[a] += c
            seen_in[a].add(r["name"])
        for a, c in r["nest"].items():
            nest[a] += c
            seen_in[a].add(r["name"])
        for a, e in r["ent"].items():
            for j in range(3):
                ent[a][j] += e[j]

    addrs = sorted(set(top) | set(nest))
    total_top, total_nest = sum(top.values()), sum(nest.values())
    # declined/bailed per function (sum over its C entries)
    f_decl = defaultdict(lambda: [0, 0, 0])
    for a, e in ent.items():
        i = find(a)
        if i >= 0:
            for j in range(3):
                f_decl[i][j] += e[j]

    nat = native_only()
    rows = []   # one per interpreted address
    for a in addrs:
        i = find(a)
        cls = classify_insn(rom, a)
        fname = funcs[i]["name"] if i >= 0 else "?"
        has_c = i >= 0 and bool(func_c[i])
        dec = f_decl[i] if i >= 0 else [0, 0, 0]
        if a in nat:
            why = "wait spin: C block in the native runner only"
        elif i < 0:
            why = "no function (data/code outside data/functions.txt)"
        elif not has_c:
            why = "no C block: function not ported"
        elif cls in BARRIER:
            why = "barrier %s between C pieces" % cls
        elif dec[1] or dec[2]:
            why = "declined/bailed tail"
        else:
            why = "gap in a ported function (no piece covers it)"
        rows.append(dict(addr=a, top=top.get(a, 0), nest=nest.get(a, 0), cls=cls, f=fname, fi=i, why=why,
                         runs=len(seen_in[a])))

    def pct(x, t):
        return "%.1f" % (100.0 * x / t) if t else "0.0"

    out = []
    w = out.append
    allc = total_top + total_nest
    w("# What still runs as 68k with `--hle all` (census)")
    w("")
    w("Generated by `tools/py/remaining68k.py` from the instruction profiles of `port/tools/ipprof.sh`")
    w("(t2-decomp#11, phase P5: run T2 with no 68k emulation). Every count is a 68k instruction the emulated CPU")
    w("*interpreted* with every C replacement on. The goal of P5 is **0** on every route; the native runner")
    w("(`T2_NATIVE=1`, docs/native.md) stops with `no C block at $ADDR` at the first of them.")
    w("")
    w("## Totals")
    w("")
    w("| routes | runs | frames | interpreted top level | interpreted nested in C | distinct addresses |")
    w("|---|---|---|---|---|---|")
    for kind in sorted(by_kind):
        k = by_kind[kind]
        w("| %s | %d | %d | %d | %d | |" % (kind, k["runs"], k["frames"], k["top"], k["nest"]))
    w("| **all** | %d | %d | **%d** | **%d** | **%d** |" % (len(runs), sum(r["frames"] for r in runs), total_top,
                                                          total_nest, len(addrs)))
    w("")
    w("*top level* = 68k code running on its own (main loop, interrupt handlers, tails of declined/bailed calls);")
    w("*nested* = 68k that a C function calls through `hle_call68k` / `hle_run68k_until`.")
    w("")

    # by reason
    groups = defaultdict(list)
    for r in rows:
        groups[r["why"]].append(r)
    w("## By reason")
    w("")
    w("| reason | addresses | functions | top-level insns | nested insns | % of all |")
    w("|---|---|---|---|---|---|")
    for why, rs in sorted(groups.items(), key=lambda kv: -sum(x["top"] + x["nest"] for x in kv[1])):
        t = sum(x["top"] for x in rs)
        n = sum(x["nest"] for x in rs)
        w("| %s | %d | %d | %d | %d | %s |" % (why, len(rs), len({x["f"] for x in rs}), t, n, pct(t + n, allc)))
    w("")

    # per reason detail: functions
    for why, rs in sorted(groups.items(), key=lambda kv: -sum(x["top"] + x["nest"] for x in kv[1])):
        w("### %s" % why)
        w("")
        byf = defaultdict(list)
        for x in rs:
            byf[x["f"]].append(x)
        w("| function | entry | addrs | top | nested | C entries reached / declined / bailed | busiest addresses |")
        w("|---|---|---|---|---|---|---|")
        flist = sorted(byf.items(), key=lambda kv: -sum(x["top"] + x["nest"] for x in kv[1]))
        for fname, xs in flist[:60]:
            fi = xs[0]["fi"]
            entry = "$%06X" % funcs[fi]["entry"] if fi >= 0 else "-"
            e = f_decl[fi] if fi >= 0 else [0, 0, 0]
            busy = sorted(xs, key=lambda x: -(x["top"] + x["nest"]))[:4]
            w("| %s | %s | %d | %d | %d | %d / %d / %d | %s |" % (
                fname, entry, len(xs), sum(x["top"] for x in xs), sum(x["nest"] for x in xs), e[0], e[1], e[2],
                ", ".join("$%06X %s %d" % (x["addr"], x["cls"], x["top"] + x["nest"]) for x in busy)))
        if len(flist) > 60:
            w("| ... %d more functions | | | | | | |" % (len(flist) - 60))
        w("")

    # barrier instruction census
    w("## Barrier instructions left to the 68k")
    w("")
    w("Instructions the pieces stop in front of (`jsr`, `bsr`, `jmp`, `rts`, ...) in functions that are otherwise C.")
    w("")
    w("| class | addresses | interpreted insns |")
    w("|---|---|---|")
    bc = defaultdict(lambda: [0, 0])
    for r in rows:
        if r["cls"] in BARRIER:
            bc[r["cls"]][0] += 1
            bc[r["cls"]][1] += r["top"] + r["nest"]
    for c, (n, t) in sorted(bc.items(), key=lambda kv: -kv[1][1]):
        w("| %s | %d | %d |" % (c, n, t))
    w("")

    # top addresses
    w("## Top 80 addresses")
    w("")
    w("| addr | function | insn | top | nested | runs | reason |")
    w("|---|---|---|---|---|---|---|")
    for r in sorted(rows, key=lambda r: -(r["top"] + r["nest"]))[:80]:
        w("| $%06X | %s | %s | %d | %d | %d | %s |" % (r["addr"], r["f"], r["cls"], r["top"], r["nest"], r["runs"], r["why"]))
    w("")

    # declined entries
    w("## Replaced entries handed back to the 68k")
    w("")
    w("C functions whose call ran (partly) as 68k: declined = an interrupt would fall inside, bailed = HLE_F_BAIL")
    w("resume at a 68k address (docs/porting.md). The native runner has no 68k to hand them to.")
    w("")
    w("| entry | name | reached | declined | bailed |")
    w("|---|---|---|---|---|")
    es = [(a, e) for a, e in ent.items() if e[1] or e[2]]
    for a, e in sorted(es, key=lambda kv: -(kv[1][1] + kv[1][2]))[:60]:
        w("| $%06X | %s | %d | %d | %d |" % (a, hle.get(a, "?"), e[0], e[1], e[2]))
    w("")

    # per-run
    w("## Per run")
    w("")
    w("| kind | run | frames | top | nested | distinct addresses |")
    w("|---|---|---|---|---|---|")
    for k, n, fr, t, ne, na in sorted(per_run):
        w("| %s | %s | %d | %d | %d | %d |" % (k, n, fr, t, ne, na))
    w("")
    wait_funcs = {x["fi"] for x in rows if x["why"].startswith("wait")}
    with open(os.path.join(d, "unported_functions.txt"), "w") as f:     # input of `piecegen.py auto`
        for fi in sorted({x["fi"] for x in rows if x["why"].startswith("no C block")} - wait_funcs):
            f.write("%06X %s\n" % (funcs[fi]["entry"], funcs[fi]["name"]))
    with open(os.path.join(d, "glue_candidates.txt"), "w") as f:     # input of tools/py/gluegen.py
        for r in rows:
            if not r["why"].startswith("wait") and r["cls"] in ("jsr", "bsr", "jmp", "rts", "rte", "bra"):
                f.write("%06X\n" % r["addr"])
    print("\n".join(out))


if __name__ == "__main__":
    main()
