#!/usr/bin/env python3
"""Merge T2_HLE_PROFILE summaries (docs/porting.md): profmerge.py [-n 40] LOG...
Reads the "PROF" lines each t2port run prints at exit, sums them and prints the C-vs-68k split
and the top functions by real-68k cycles (idle wait loops are listed apart)."""
import argparse
import collections

IDLE = {"WaitVBlank", "FramePaceWait", "sub_01DC4E"}

ap = argparse.ArgumentParser()
ap.add_argument("logs", nargs="+")
ap.add_argument("-n", type=int, default=40)
a = ap.parse_args()
tot = [0] * 6
fn = collections.OrderedDict()
for path in a.logs:
    for line in open(path, errors="replace"):
        p = line.split()
        if len(p) >= 8 and p[0] == "PROF" and p[1] == "tot":
            tot = [x + int(y) for x, y in zip(tot, p[2:8])]
        elif len(p) == 9 and p[0] == "PROF" and p[1] == "fn":
            e = fn.setdefault(p[2], [p[3], 0, 0, 0, 0, 0])
            e[1] += int(p[4])
            e[2] += int(p[5])
            e[3] += int(p[6])
            e[4] = int(p[7])
            e[5] += int(p[8])
frames, real, idle, c, nested, other = tot
T = (real + idle + c) or 1
busy = (real + c) or 1
print("frames %d, %.1f M 68k cycles" % (frames, T / 7e6))
print("C %.2f%%  68k real %.2f%%  idle %.2f%%  (nested 68k inside C: %.2f%%)" % (
    100 * c / T, 100 * real / T, 100 * idle / T, 100 * nested / T))
print("pure C (nested 68k excluded) %.2f%%; all 68k code (real + nested) %.2f%% of busy" % (
    100 * (c - nested) / T, 100 * (real + nested) / busy))
print("busy only (idle excluded): C %.2f%%  68k %.2f%%" % (100 * c / busy, 100 * real / busy))
dec = sum(v[1] for k, v in fn.items() if v[4] and v[0] not in IDLE)
print("HLE'd functions that ran as 68k anyway (declined: IRQ inside the call): %.2f%% of all, %.2f%% of busy" % (
    100 * dec / T, 100 * dec / busy))
rows = sorted(fn.items(), key=lambda kv: -kv[1][1])
print("\n| addr | name | Mcyc | % of all | % of busy | calls | declined |\n|---|---|---|---|---|---|---|")
n = 0
for addr, (name, m, nm, calls, hle, decl) in rows:
    if name in IDLE or n >= a.n:
        continue
    n += 1
    print("| $%s | %s | %.2f | %.2f | %.2f | %d | %s |" % (addr, name, m / 7e6, 100 * m / T, 100 * m / busy, calls, "%d" % decl if hle else ""))
print("\nidle:")
for addr, (name, m, nm, calls, hle, decl) in rows:
    if name in IDLE:
        print("  $%s %s %.2f Mcyc (%.2f%% of all)" % (addr, name, m / 7e6, 100 * m / T))

rows = sorted(fn.items(), key=lambda kv: -kv[1][2])
print("\n68k run nested inside C calls (hle_call68k), top %d:" % a.n)
for addr, (name, m, nm, calls, hle, decl) in rows[:a.n]:
    if nm:
        print("  $%s %s %.2f Mcyc (%.2f%% of all)" % (addr, name, nm / 7e6, 100 * nm / T))
