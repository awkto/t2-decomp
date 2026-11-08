#!/usr/bin/env python3
"""Aggregate the logs of a native census run (T2_NATIVE=1 T2_NATIVE_FALLBACK=1 port/tools/ipprof.sh, docs/native.md).

With T2_NATIVE_FALLBACK=1 the native runner executes an address that has no C block on the 68k core (one instruction)
and prints `native-fallback: $ADDR COUNT` at exit, so one run lists every address a route needs C for.  This groups the
addresses of all logs by function (data/functions.txt) and prints, per function, the routes that reach it, the number of
addresses and the instruction count.

usage: python3 -I tools/py/nativecensus.py [runs/native/work] [-n 60]
"""
import collections
import glob
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def functions():
    out = []
    for line in open(os.path.join(ROOT, "data/functions.txt")):
        if line.startswith("#") or not line.strip():
            continue
        main_, _, comment = line.partition("#")
        f = main_.split()
        rngs = [(int(f[0], 16), int(f[0], 16) + int(f[1]))]
        for m in re.finditer(r"\+([0-9a-f]+):(\d+)", comment):
            rngs.append((int(m.group(1), 16), int(m.group(1), 16) + int(m.group(2))))
        out.append((int(f[0], 16), f[2], rngs))
    return out


def strict(d):
    """the logs of a strict native census (T2_NATIVE=1 port/tools/ipprof.sh): where each run stopped, in markdown"""
    ROUTES = ["coldboot_gate", "ringstate_gate", "pause_gate", "levelflow_gate", "screens_gate"]
    runs = {}
    for lg in sorted(glob.glob(os.path.join(d, "*.log"))):
        name = os.path.basename(lg)[:-4]
        txt = open(lg, errors="replace").read()
        m = re.search(r"native: no C block at \$([0-9A-F]{6}) after (\d+) frames", txt)
        done = re.search(r"(\d+) frames in [\d.]+ s", txt)
        if m:
            runs[name] = ("stop", int(m.group(1), 16), int(m.group(2)))
        elif done:
            runs[name] = ("done", 0, int(done.group(1)))
        else:
            other = [l for l in txt.splitlines() if l.startswith(("hle:", "native:")) and "active" not in l]
            runs[name] = ("other", 0, 0, other[-1][:80] if other else "no output")
    fn = functions()

    def fname(a):
        for e, n, rngs in fn:
            if any(lo <= a < hi for lo, hi in rngs):
                return n
        return "?"
    print("| route | frames | first stop |")
    print("|---|---|---|")
    for r in ROUTES:
        v = runs.get(r)
        if not v:
            continue
        print("| %s | %d | %s |" % (r, v[2], "none: ran to the end" if v[0] == "done" else ("$%06X %s" % (v[1], fname(v[1])) if v[0] == "stop" else v[3])))
    states = {k: v for k, v in runs.items() if k not in ROUTES}
    done = [k for k, v in states.items() if v[0] == "done"]
    print()
    print("States (%d runs of 3000 frames): %d ran to the end with no 68k instruction." % (len(states), len(done)))
    by = collections.defaultdict(list)
    for k, v in states.items():
        if v[0] == "stop":
            by[(v[1], fname(v[1]))].append(v[2])
        elif v[0] == "other":
            by[(0, v[3])].append(0)
    print()
    print("| first stop | runs | frames before it (min / median / max) |")
    print("|---|---|---|")
    for (a, nm), fr in sorted(by.items(), key=lambda kv: -len(kv[1])):
        fr.sort()
        print("| %s %s | %d | %d / %d / %d |" % ("$%06X" % a if a else "", nm, len(fr), fr[0], fr[len(fr) // 2], fr[-1]))


def main():
    if "--strict" in sys.argv:
        return strict(sys.argv[sys.argv.index("--strict") + 1])
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    d = args[0] if args else os.path.join(ROOT, "runs/native/work")
    top = 60
    if "-n" in sys.argv:
        top = int(sys.argv[sys.argv.index("-n") + 1])
    fns = functions()

    def fn_of(a):
        for e, n, rngs in fns:
            for lo, hi in rngs:
                if lo <= a < hi:
                    return e, n
        return a, "(no function)"

    addrs = collections.Counter()
    entries = collections.Counter()
    routes = collections.defaultdict(set)
    ends = collections.Counter()
    nrun = 0
    for lg in sorted(glob.glob(os.path.join(d, "*.log"))):
        name = os.path.basename(lg)[:-4]
        txt = open(lg, errors="replace").read()
        if "native:" not in txt and "native-fallback" not in txt:
            continue
        nrun += 1
        for m in re.finditer(r"native-fallback: \$([0-9A-F]{6}) (\d+) (\d+)", txt):
            a = int(m.group(1), 16)
            addrs[a] += int(m.group(2))
            routes[a].add(name)
            if int(m.group(3)):
                entries[a] += int(m.group(3))
        m = re.search(r"native: (\d+) C blocks run, (\d+) instructions", txt)
        if m:
            ends["ran"] += 1
        else:
            ends["no summary (crashed / timed out)"] += 1
    per = collections.defaultdict(lambda: [0, set(), set()])
    for a, n in addrs.items():
        e, nm = fn_of(a)
        r = per[(e, nm)]
        r[0] += n
        r[1].add(a)
        r[2] |= routes[a]
    with open(os.path.join(d, "..", "entries.txt"), "w") as f:      # input of bootgen.py auto
        for a, n in sorted(entries.items()):
            f.write("%06X %d\n" % (a, n))
    print("%d runs, %d distinct addresses without a C block, %d instructions on the 68k core" %
          (nrun, len(addrs), sum(addrs.values())))
    for k, v in ends.items():
        print("  %s: %d" % (k, v))
    print()
    print("| function | entry | routes | addrs | insns | first addresses |")
    print("|---|---|---|---|---|---|")
    for (e, nm), (n, aset, rs) in sorted(per.items(), key=lambda kv: -len(kv[1][2]) * 1000000 - kv[1][0])[:top]:
        first = ", ".join("$%06X" % a for a in sorted(aset)[:4])
        print("| %s | $%06X | %d | %d | %d | %s |" % (nm, e, len(rs), len(aset), n, first))


if __name__ == "__main__":
    main()
