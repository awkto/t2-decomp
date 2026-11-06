#!/usr/bin/env python3
"""Decline-window bound check (docs/porting.md, T2_HLE_BOUNDS): run `t2port --hle all` with
T2_HLE_BOUNDS=1 from savestates using lockstep.py's input script and merge the per-function
tables (max observed span, worst obs/bound ratio, OVER/near flags).

    python3 -I port/tools/bounds.py [--frames 3000] [--states "a b c"]   (default: states/*.state)
"""
import argparse, glob, os, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lockstep

ROOT = lockstep.ROOT


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=3000)
    ap.add_argument("--states", default="")
    ap.add_argument("--bin", default=os.path.join(ROOT, "build", "port", "t2port"))
    a = ap.parse_args()
    states = a.states.split() or sorted(os.path.basename(p)[:-6] for p in glob.glob(os.path.join(ROOT, "states", "*.state")))
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", T2_HLE_BOUNDS="1")
    agg = {}   # name -> [bound, maxobs, calls, over, worst_ratio, worst_str, state_of_max]
    with tempfile.TemporaryDirectory() as tmp:
        for st in states:
            script = os.path.join(tmp, st + ".txt")
            lockstep.write_script(script, st, a.frames)
            r = subprocess.run([a.bin, "--script", script, "--outdir", os.path.join(tmp, "o"), "--mute", "--no-vsync",
                                "--hle", "all"], cwd=ROOT, env=env, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, timeout=600)
            for line in r.stderr.decode(errors="replace").splitlines():
                if not line.startswith("hle bounds:") or "max obs" in line:
                    continue
                p = line.split()[2:]
                name, bound, mx, calls = p[0], int(p[1]), int(p[2]), int(p[3])
                w = p[4].split("/")
                ratio = int(w[0]) / max(int(w[1]), 1)
                e = agg.setdefault(name, [0, 0, 0, 0, 0.0, "", ""])
                e[0] = max(e[0], bound)
                if mx > e[1]:
                    e[1], e[6] = mx, st
                e[2] += calls
                if ratio > e[4]:
                    e[4], e[5] = ratio, p[4]
                if "OVER" in p[5:]:
                    e[3] += 1
            print("done", st, file=sys.stderr, flush=True)
    print("%-22s %8s %9s %9s %12s %s" % ("function", "bound", "max obs", "calls", "worst o/b", "max in"))
    for n, e in sorted(agg.items(), key=lambda kv: -kv[1][4]):
        flag = "OVER" if e[3] else ("near" if e[4] > 0.8 else "")
        print("%-22s %8d %9d %9d %12s %s %s" % (n, e[0], e[1], e[2], e[5], e[6], flag))


if __name__ == "__main__":
    main()
