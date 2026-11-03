#!/usr/bin/env python3
"""Diff a C replacement's instruction timeline against the original 68k, call by call.

    port/tools/seqcmp.py FUNC [--state gun_05] [--frames 1000] [--spec SPEC] [--rows 8]

Runs t2port twice from a states/ savestate with the lockstep gate's input: the C run (--hle SPEC,
default FUNC) and the original (T2_HLE_ORIG=FUNC, so FUNC runs as 68k).  Both run with T2_HLE_SEQ=1,
which makes hle.c log one `seq NAME: c1 c2 ...` line per top-level call: the instruction costs the C
function passed to hle_insn/hle_post, resp. what each original instruction really cost (bus refresh
removed).  The calls are paired in order; the first mismatching rows are shown with the position of
the first differing instruction.  Rows after the first divergence of the two runs are noise; if the
timeline is exact every row matches.
"""
import argparse
import importlib.util
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def load_lockstep():
    sp = importlib.util.spec_from_file_location("lockstep", os.path.join(ROOT, "port", "tools", "lockstep.py"))
    m = importlib.util.module_from_spec(sp)
    sp.loader.exec_module(m)
    return m


def run(binary, script, outdir, spec, env):
    cmd = [binary, "--script", script, "--outdir", outdir, "--mute", "--no-vsync", "--hle", spec]
    r = subprocess.run(cmd, cwd=ROOT, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=300)
    return [l for l in r.stderr.decode(errors="replace").splitlines() if l.startswith("seq ")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("func")
    ap.add_argument("--state", default="gun_05")
    ap.add_argument("--frames", type=int, default=1000)
    ap.add_argument("--spec", default=None)
    ap.add_argument("--rows", type=int, default=8)
    a = ap.parse_args()
    ls = load_lockstep()
    d = os.path.join(ROOT, "runs", "seqcmp", a.state)
    os.makedirs(d, exist_ok=True)
    script = os.path.join(d, "script.txt")
    ls.write_script(script, a.state, a.frames)
    binary = os.path.join(ROOT, "build", "port", "t2port")
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", T2_HLE_SEQ="1",
               T2_HLE_NODECLINE="1")   # a declined call (run as 68k) has no C timeline to compare
    spec = a.spec or a.func
    c = run(binary, script, d, spec, env)
    o = run(binary, script, d, spec, dict(env, T2_HLE_ORIG=a.func))
    c = [l for l in c if l.startswith("seq %s:" % a.func)]
    o = [l for l in o if l.startswith("seq %s:" % a.func)]
    bad = 0
    for i, (x, y) in enumerate(zip(o, c)):
        xs, ys = x.split(":", 1)[1].split(), y.split(":", 1)[1].split()
        if xs != ys:
            bad += 1
            if bad <= a.rows:
                k = next((j for j in range(min(len(xs), len(ys))) if xs[j] != ys[j]), min(len(xs), len(ys)))
                print("call %d: orig %d insns / %d cyc, C %d insns / %d cyc; first diff at insn %d: orig %s C %s"
                      % (i, len(xs), sum(map(int, xs)), len(ys), sum(map(int, ys)), k,
                         xs[max(0, k - 2):k + 3], ys[max(0, k - 2):k + 3]))
    print("%s %s: %d calls (orig %d, C %d), %d differ" % (a.func, a.state, min(len(o), len(c)), len(o), len(c), bad))
    return 1 if bad or len(o) != len(c) else 0


if __name__ == "__main__":
    sys.exit(main())
