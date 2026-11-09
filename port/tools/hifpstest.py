#!/usr/bin/env python3
"""High frame rate test (docs/hifps.md): for every savestate (and route script), run t2port
twice with the lockstep input, once at 60 Hz and once with --fps RATE (the in-betweens are drawn and
counted in the batch run, nothing is presented), and require

  * the per-frame machine hashes (--framehash: work RAM, VRAM, CRAM, VSRAM, VDP registers) to be
    identical: the feature is presentation only;
  * (informational) the hifps counters: frames interpolated / still / held, sprite entries matched.

  port/tools/hifpstest.sh                        # all states, 1500 frames, 120 Hz
  port/tools/hifpstest.sh --states "gun_01 l8_hit" --frames 600 --fps 240
"""
import argparse
import glob
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lockstep  # noqa: E402

ROOT = lockstep.ROOT


def run(args, name, script, extra, outdir, hashf):
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", T2_HIFPS_STATS="1")
    cmd = ["nice", "-n", "19", args.bin, "--script", script, "--outdir", outdir, "--mute", "--no-vsync",
           "--hle", args.hle, "--framehash", hashf] + extra
    r = subprocess.run(cmd, cwd=ROOT, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=600)
    return r.returncode, r.stderr.decode(errors="replace")


def one(args, name):
    out = os.path.join(ROOT, "runs", "hifps", name)
    os.makedirs(out, exist_ok=True)
    script = os.path.join(out, "script.txt")
    lockstep.write_script(script, name, args.frames)
    h_off, h_on = os.path.join(out, "off.txt"), os.path.join(out, "on.txt")
    rc0, err0 = run(args, name, script, [], out, h_off)
    rc1, err1 = run(args, name, script, ["--fps", str(args.fps)], out, h_on)
    same = rc0 == 0 and rc1 == 0 and open(h_off, "rb").read() == open(h_on, "rb").read()
    stats = [l for l in err1.splitlines() if l.startswith("hifps:")]
    return name, same, stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--states", default="")
    ap.add_argument("--frames", type=int, default=1500)
    ap.add_argument("--fps", type=int, default=120)
    ap.add_argument("--hle", default="all")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--bin", default=os.path.join(ROOT, "build", "port", "t2port"))
    ap.add_argument("-v", action="store_true")
    args = ap.parse_args()
    names = args.states.split() if args.states else sorted(
        os.path.basename(p)[:-6] for p in glob.glob(os.path.join(ROOT, "states", "*.state")))
    bad = 0
    tot = dict(frames=0, interp=0, still=0, held=0, ents=0, matched=0, pieces=0, pm=0, unknown=0)
    with ThreadPoolExecutor(args.jobs) as ex:
        for name, same, stats in ex.map(lambda n: one(args, n), names):
            txt = "\n".join(stats)
            m = re.search(r"(\d+) frames: (\d+) interpolated, (\d+) still, held: ([^\n]*)", txt)
            if m:
                tot["frames"] += int(m.group(1))
                tot["interp"] += int(m.group(2))
                tot["still"] += int(m.group(3))
                tot["held"] += sum(int(x) for x in re.findall(r"(\d+) [a-z]", m.group(4)))
            m = re.search(r"(\d+) of (\d+) sprite entries matched.*?(\d+) of (\d+) pieces moved.*?unknown (\d+)", txt)
            if m:
                tot["matched"] += int(m.group(1))
                tot["ents"] += int(m.group(2))
                tot["pm"] += int(m.group(3))
                tot["pieces"] += int(m.group(4))
                tot["unknown"] += int(m.group(5))
            if not same:
                bad += 1
            if args.v or not same:
                print("%-12s %s" % (name, "identical" if same else "STATE DIFFERS"))
                for s in stats:
                    print("    " + s)
    print("hifpstest: %d states, machine state identical to 60 Hz in %d" % (len(names), len(names) - bad))
    print("hifpstest: %d frames: %d interpolated, %d still, %d held; sprite entries matched %d of %d; owners unknown on %d frames" % (
        tot["frames"], tot["interp"], tot["still"], tot["held"], tot["matched"], tot["ents"], tot["unknown"]))
    print("hifpstest: PASS" if not bad else "hifpstest: FAIL")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
