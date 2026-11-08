#!/usr/bin/env python3
"""Render gate: the C renderer (docs/render.md) must draw every frame pixel-identical to GPGX.

Runs t2port in compare mode (T2_RENDER=compare: both renderers draw every frame, the first
differing pixel is reported) over

  * every savestate in states/ (or --states) for --frames frames of the lockstep input, and
  * the five route scripts (pause, levelflow, screens, coldboot, ringstate),

and requires 0 differing frames. Prints one line per run and a summary; exit 0 only if all
are identical.

    port/tools/rendergate.sh [--frames 3000] [--hle all] [--jobs 4] [--states "a b"] [--routes "..."|none]
"""
import argparse, glob, os, re, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lockstep

ROUTES = ["pause_gate", "levelflow_gate", "screens_gate", "coldboot_gate", "ringstate_gate"]
SUMMARY = re.compile(r"rendercmp: (\d+) frames compared, (\d+) differ, (\d+) pixels differ")


def run_one(name, script, a, tmp):
    out = os.path.join(tmp, name.replace(":", "_"))
    os.makedirs(out, exist_ok=True)
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", T2_RENDER="compare")
    cmd = [a.bin, "--script", script, "--outdir", out, "--mute", "--no-vsync", "--hle", a.hle]
    try:
        r = subprocess.run(["nice", "-n", "19"] + cmd, cwd=lockstep.ROOT, env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, timeout=a.timeout)
    except subprocess.TimeoutExpired:
        return name, 0, -1, "TIMEOUT", ""
    err = r.stderr.decode(errors="replace")
    m = SUMMARY.search(err)
    first = [l for l in err.splitlines() if l.startswith("rendercmp: frame")][:3]
    if r.returncode != 0 or not m:
        return name, 0, -1, "FAILED rc=%d" % r.returncode, "\n".join(err.splitlines()[-5:])
    return name, int(m.group(1)), int(m.group(2)), "", "\n".join(first)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=3000)
    ap.add_argument("--hle", default="all")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--states", default=None, help="space separated names (default: every states/*.state)")
    ap.add_argument("--routes", default=" ".join(ROUTES), help="route script names under port/scripts/, or 'none'")
    ap.add_argument("--bin", default=os.path.join(lockstep.ROOT, "build", "port", "t2port"))
    ap.add_argument("--timeout", type=int, default=600)
    a = ap.parse_args()
    if not os.path.exists(a.bin):
        sys.stderr.write("%s: not built (run port/tools/build.sh)\n" % a.bin)
        return 2
    states = a.states.split() if a.states else sorted(
        os.path.basename(p)[:-6] for p in glob.glob(os.path.join(lockstep.ROOT, "states", "*.state")))
    routes = [] if a.routes == "none" else a.routes.split()
    with tempfile.TemporaryDirectory() as tmp:
        jobs = []
        for st in states:
            sp = os.path.join(tmp, st + ".txt")
            lockstep.write_script(sp, st, a.frames)
            jobs.append(("state:" + st, sp))
        for r in routes:
            jobs.append(("route:" + r, os.path.join(lockstep.ROOT, "port", "scripts", r + ".txt")))
        with ThreadPoolExecutor(max_workers=a.jobs) as ex:
            res = list(ex.map(lambda j: run_one(j[0], j[1], a, tmp), jobs))
    ok = {"state": [0, 0, 0], "route": [0, 0, 0]}      # runs identical, runs, frames
    bad = 0
    for name, frames, differ, why, detail in res:
        kind = name.split(":")[0]
        ok[kind][1] += 1
        ok[kind][2] += max(frames, 0)
        if differ == 0:
            ok[kind][0] += 1
            print("%-28s %d frames identical" % (name, frames))
        else:
            bad += 1
            print("%-28s DIFFERS: %s (%d of %d frames)" % (name, why or "pixels", differ, frames))
            if detail:
                print("    " + detail.replace("\n", "\n    "))
    print("rendergate: states %d/%d identical (%d frames), routes %d/%d identical (%d frames)" % (
        ok["state"][0], ok["state"][1], ok["state"][2], ok["route"][0], ok["route"][1], ok["route"][2]))
    print("rendergate: %s" % ("PASS" if not bad else "FAIL (%d run(s) differ)" % bad))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
