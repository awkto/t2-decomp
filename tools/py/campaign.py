#!/usr/bin/env python3
"""Coverage campaign driver (run from the repository root; on a VM for long runs via tools/py/vmjob.sh).

    campaign.py run MODE [--seeds K] [--first N] [--frames F] [--jobs J]

MODE is attract, gun or pad (see tools/py/fuzzscript.py).  Each job runs build/harness (or
tools/harness/harness) on a generated script in runs/<mode><seed>/ and its cov.bin is OR-ed into
runs/cov_acc.bin and deleted.  Merge the result with:

    tools/py/mergecov.py runs/cov_acc.bin
"""
import argparse
import concurrent.futures as cf
import os
import shutil
import subprocess
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fuzzscript  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
os.chdir(ROOT)
ACC = "runs/cov_acc.bin"
lock = threading.Lock()


def harness():
    return "build/harness" if os.path.exists("build/harness") else "tools/harness/harness"


def acc_merge(path):
    d = open(path, "rb").read()
    n = len(d)
    with lock:
        a = open(ACC, "rb").read() if os.path.exists(ACC) else bytes(n)
        v = int.from_bytes(a, "big") | (int.from_bytes(d, "big") & int.from_bytes(b"\x03" * n, "big"))
        open(ACC, "wb").write(v.to_bytes(n, "big"))


def job(mode, seed, frames):
    out = f"runs/{mode}{seed}"
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    sp = os.path.join(out, "script.txt")
    open(sp, "w").write("\n".join(fuzzscript.gen(mode, frames, seed)) + "\n")
    env = dict(os.environ, GUN="1")
    subprocess.run(["nice", "-n", "19", harness(), "ext/gpgx/genesis_plus_gx_libretro.so", "rom/baserom.md", sp, out],
                   capture_output=True, text=True, timeout=3600, env=env)
    cov = os.path.join(out, "cov.bin")
    if os.path.exists(cov):
        acc_merge(cov)
        os.remove(cov)
    return f"{mode}{seed} done"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["run"])
    ap.add_argument("mode", choices=["attract", "gun", "pad"])
    ap.add_argument("--seeds", type=int, default=1)
    ap.add_argument("--first", type=int, default=1)
    ap.add_argument("--frames", type=int, default=20000)
    ap.add_argument("--jobs", type=int, default=2)
    a = ap.parse_args()
    os.makedirs("runs", exist_ok=True)
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for r in ex.map(lambda s: job(a.mode, s, a.frames), range(a.first, a.first + a.seeds)):
            print(r, flush=True)


if __name__ == "__main__":
    main()
