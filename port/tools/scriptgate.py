#!/usr/bin/env python3
"""Run one t2port script with --hle none and --hle SPEC and compare the per-frame hashes
(the lockstep gate for a hand-written route, e.g. port/scripts/pause_gate.txt).

    python3 -I port/tools/scriptgate.py SCRIPT [--hle all]
"""
import argparse, os, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lockstep


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("script")
    ap.add_argument("--hle", default="all")
    ap.add_argument("--bin", default=os.path.join(lockstep.ROOT, "build", "port", "t2port"))
    a = ap.parse_args()
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    with tempfile.TemporaryDirectory() as tmp:
        ref = lockstep.read_hashes(lockstep.run(a.bin, a.script, os.path.join(tmp, "n"), "none", os.path.join(tmp, "n.h"), env))
        got = lockstep.read_hashes(lockstep.run(a.bin, a.script, os.path.join(tmp, "s"), a.hle, os.path.join(tmp, "s.h"), env))
    d = lockstep.first_diff(ref, got)
    if d:
        print("%s: DIVERGES at frame %s (%s)" % (a.script, d[0], ",".join(d[1])))
        return 1
    print("%s: %d frames identical (--hle %s)" % (a.script, len(ref), a.hle))
    return 0


if __name__ == "__main__":
    sys.exit(main())
