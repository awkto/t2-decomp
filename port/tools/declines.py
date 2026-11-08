#!/usr/bin/env python3
"""Decline table: run --hle all with T2_HLE_DECLINES=1 T2_HLE_PROFILE=1 over some states
(lockstep input script) and print per function and reason how often a C replacement fell back
to the original 68k, then the profmerge C/68k split.   declines.py [--frames N] [STATE...]"""
import argparse, collections, os, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lockstep as L

ap = argparse.ArgumentParser()
ap.add_argument("--frames", type=int, default=3000)
ap.add_argument("states", nargs="*", default=L.DEFAULT_STATES + ["pad_05", "gun_03", "2p_gun_04"])
a = ap.parse_args()
env = dict(os.environ, T2_HLE_DECLINES="1", T2_HLE_PROFILE="1")
tab = collections.OrderedDict()
logs = []
tmp = tempfile.mkdtemp()
for st in a.states:
    sc = os.path.join(tmp, st + ".txt")
    L.write_script(sc, st, a.frames)
    out = os.path.join(tmp, st)
    os.makedirs(out, exist_ok=True)
    r = subprocess.run([L.ROOT + "/build/port/t2port", "--script", sc, "--outdir", out, "--mute", "--no-vsync", "--hle", "all"],
                       cwd=L.ROOT, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=300)
    log = os.path.join(tmp, st + ".log")
    open(log, "wb").write(r.stderr)
    logs.append(log)
    for line in r.stderr.decode(errors="replace").splitlines():
        p = line.split()
        if len(p) == 12 and p[0] == "declines:" and p[1] != "function":
            e = tab.setdefault(p[1], [0] * 9)
            for i in range(9):
                e[i] += int(p[2 + i] if i < 3 else p[3 + i - 0])
print("%-20s %8s %8s %8s | %6s %6s %6s %6s %6s %6s" % ("function", "frameend", "irqwin", "pendirq", "dma", "vint", "vclr", "hint", "gun", "other"))
for k, e in sorted(tab.items(), key=lambda kv: -sum(kv[1])):
    print("%-20s %8d %8d %8d | %6d %6d %6d %6d %6d %6d" % (k, *e[:3], *e[3:]))
subprocess.run([sys.executable, "-I", os.path.join(L.ROOT, "port/tools/profmerge.py"), "-n", "12"] + logs)
