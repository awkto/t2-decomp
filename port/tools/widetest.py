#!/usr/bin/env python3
"""Widescreen tests (issue #13): see port/tools/widetest.sh and docs/widescreen.md.

    widetest.py mouse PEEKLOG            check the pointer / crosshair / trigger behaviour of port/scripts/wide/mouse.txt
    widetest.py state [--bin B]          game state with widescreen on == off, every frame
    widetest.py shots [--bin B] OUTDIR   screenshots (PNG) with the feature on
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "port", "tools"))
import lockstep  # noqa: E402

STATES = ["gun_01", "gun_02", "gun_04", "gun_05", "gun_07", "title"]


def parse_log(path):
    """peek.log -> dict label -> list of lines"""
    out = {}
    for line in open(path):
        m = re.match(r"(\S+?):?\s+(.*)$", line.strip())
        if m:
            out.setdefault(m.group(1), []).append(m.group(2))
    return out


def mouse(log):
    ok = True
    lines = open(log).read()

    def check(name, cond, detail):
        nonlocal ok
        print("  %-62s %s  %s" % (name, "ok" if cond else "FAIL", detail))
        ok = ok and cond

    def xh(label):                       # crosshair LABEL: gun_x gun_y p1_x p1_y sprite_x sprite_y
        m = re.search(r"^\d+ %s (-?\d+) (-?\d+) (-?\d+) (-?\d+) (-?\d+) (-?\d+)" % re.escape(label), lines, re.M)
        return tuple(int(v) for v in m.groups()) if m else (None,) * 6

    def peek(label):
        m = re.search(r"^\d+ %s ([0-9a-fA-F]+)" % re.escape(label), lines, re.M)
        return int(m.group(1), 16) if m else None

    c = xh("centre")
    check("pointer at frame x 200 -> game x 160 (aim within 2)", c[2] is not None and abs(c[2] - 160) <= 2, "p1_x=%s" % c[2])
    e = xh("left-edge")
    check("pointer at frame x 40 -> the game's left edge", e[2] is not None and e[2] <= 4, "p1_x=%s" % e[2])
    r = xh("right-edge")
    check("pointer at frame x 356 -> game x 316 (the right edge)", r[2] is not None and abs(r[2] - 316) <= 2, "p1_x=%s" % r[2])
    i = xh("inside-100-150")
    check("pointer at frame (100,150) -> game (60,150)", i[2] is not None and abs(i[2] - 60) <= 2 and abs(i[3] - 150) <= 1,
          "p1=(%s,%s)" % (i[2], i[3]))
    lm = xh("left-margin")
    check("pointer over the left margin: crosshair waits at the left edge", lm[2] is not None and lm[2] <= 4, "p1_x=%s" % lm[2])
    rm = xh("right-margin")
    check("pointer over the right margin: crosshair waits at the right edge", rm[2] is not None and rm[2] >= 316, "p1_x=%s" % rm[2])
    s0, s1 = peek("shot-before"), peek("shot-after-margin-click")
    check("trigger over the left margin fires nothing (no shot resolved)", s0 is not None and s0 == s1, "%s -> %s" % (s0, s1))
    m0, m1 = peek("missiles-before"), peek("missiles-after-margin-right-click")
    check("missile button over the left margin fires nothing", m0 is not None and m0 == m1, "%s -> %s" % (m0, m1))
    s2 = peek("shot-after-right-margin-click")
    check("trigger over the right margin fires nothing", s2 is not None and s2 == s0, "%s" % s2)
    s3 = peek("shot-after-field-click")
    check("the same click over the playfield fires (a shot is resolved)", s3 is not None and s3 != s0, "%s" % s3)
    m2 = peek("missiles-after-field-right-click")
    check("the missile button over the playfield fires (count drops)", m0 is not None and m2 is not None and m2 < m0, "%s -> %s" % (m0, m2))
    return ok


def state(binary):
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", T2_NATIVE="1")
    env.pop("T2_WIDE", None)
    d = os.path.join(ROOT, "runs", "widetest", "state")
    os.makedirs(d, exist_ok=True)
    bad = 0
    for st in STATES:
        script = os.path.join(d, st + ".txt")
        lockstep.write_script(script, st, 3000)
        hs = {}
        for tag, opt in (("off", "off"), ("wide", "16:9"), ("wide21", "21:9")):
            h = os.path.join(d, "%s.%s.hash" % (st, tag))
            cmd = [binary, "--script", script, "--outdir", d, "--mute", "--no-vsync", "--hle", "all",
                   "--widescreen", opt, "--framehash", h]
            r = subprocess.run(cmd, cwd=ROOT, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=300)
            if r.returncode:
                sys.stderr.write(r.stderr.decode(errors="replace"))
                raise RuntimeError("t2port failed")
            hs[tag] = lockstep.read_hashes(h)
        for tag in ("wide", "wide21"):
            fd = lockstep.first_diff(hs["off"], hs[tag])
            same = fd is None and all(a[2] == b[2] for a, b in zip(hs["off"], hs[tag]))
            print("%-8s %-7s %s" % (st, tag, "%d frames identical" % len(hs[tag]) if same else
                                    "DIVERGES at frame %s" % (fd[0] if fd else "?")))
            bad += 0 if same else 1
    return bad == 0


def shots(binary, outdir):
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", T2_NATIVE="1")
    os.makedirs(outdir, exist_ok=True)
    plan = [("gun_01", 700), ("gun_02", 1200), ("gun_03", 900), ("gun_04", 1500), ("gun_05", 1500),
            ("gun_06", 600), ("gun_07", 1500), ("gun_08", 600), ("title", 60), ("hk_a", 200)]
    for st, n in plan:
        script = os.path.join(outdir, "shot_%s.txt" % st)
        with open(script, "w") as f:
            f.write("load states/%s.state\nrun %d\nshot wide_%s\nfmouse 320 100\nrun 3\nshot wide_%s_aim\n" % (st, n, st, st))
        cmd = [binary, "--script", script, "--outdir", outdir, "--mute", "--no-vsync", "--hle", "all",
               "--widescreen", "16:9"]
        subprocess.run(cmd, cwd=ROOT, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300)
    ppms = sorted(f for f in os.listdir(outdir) if f.endswith(".ppm"))
    for f in ppms:
        subprocess.run(["convert", os.path.join(outdir, f), os.path.join(outdir, f[:-4] + ".png")], check=False)
        os.remove(os.path.join(outdir, f))
    print("%d screenshots in %s" % (len(ppms), outdir))
    return len(ppms) > 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("what", choices=["mouse", "state", "shots"])
    ap.add_argument("arg", nargs="?")
    ap.add_argument("--bin", default=os.path.join(ROOT, "build", "port", "t2port"))
    a = ap.parse_args()
    if a.what == "mouse":
        ok = mouse(a.arg)
    elif a.what == "state":
        ok = state(a.bin)
    else:
        ok = shots(a.bin, a.arg)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
