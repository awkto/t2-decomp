#!/usr/bin/env python3
"""Generate and analyse the harness scripts for the Menacer measurements in docs/gun.md.

All generated scripts start from the "l1" state written by tools/harness/scripts/gun_boot.txt, so
run them with the same outdir, with GUN=1 GUNMODE=lightgun.

  gunscripts.py grid [STEP] > grid.txt     aim grid: every STEP px (default 16) over 320x224, plus
                                           the right/bottom edge; reload l1 for every point and
                                           settle 24 frames (the game averages 16 samples)
  gunscripts.py xsweep [Y] > xs.txt        every x 0..319 at one y (default 100)
  gunscripts.py tap > tap.txt              fire rate: tap 1 on/1 off, 2 on/2 off, hold (600 frames each)
  gunscripts.py lag > lag.txt              step and ramp response of the crosshair
  gunscripts.py report OUTDIR              analyse OUTDIR/peek.log (grid/xsweep) and OUTDIR/watch.log (tap)
"""
import collections
import sys

HEAD = "load l1\n"


def grid(step=16):
    out = []
    ys = list(range(0, 224, step)) + [223]
    xs = list(range(0, 320, step)) + [319]
    for y in ys:
        for x in xs:
            out.append(HEAD + f"gun {x} {y} 0 24\necho P {x} {y}\n"
                       "peek X FF8004 2\npeek Y FF8006 2\npeek RAWY FF9F70 2\npeek NLATCH FF9F74 1\n")
    return "".join(out)


def xsweep(y=100):
    return "".join(HEAD + f"gun {x} {y} 0 20\necho P {x} {y}\npeek X FF8004 2\npeek Y FF8006 2\n"
                   for x in range(320))


def tap():
    s = [HEAD, "watch FFA0A2 1\necho TAP1 1-on-1-off\n", "gun 160 60 1 1\ngun 160 60 0 1\n" * 300,
         "unwatch\npeek gunpower FF801C 2\n", HEAD, "watch FFA0A2 1\necho TAP2 2-on-2-off\n",
         "gun 160 60 1 2\ngun 160 60 0 2\n" * 150, "unwatch\npeek gunpower FF801C 2\n",
         HEAD, "watch FFA0A2 1\nwatch FF8012 2\necho HOLD\n", "gun 160 60 1 600\n",
         "unwatch\npeek gunpower FF801C 2\n"]
    return "".join(s)


def lag():
    s = [HEAD, "gun 60 60 0 30\necho STEP 60,60->260,160\npeekeach X FF8004 2\npeekeach Y FF8006 2\n",
         "gun 260 160 0 20\npeekeach off\n",
         "gun 100 100 0 30\necho STEP 100,100->104,102\npeekeach X FF8004 2\npeekeach Y FF8006 2\n",
         "gun 104 102 0 20\npeekeach off\n",
         "gun 100 100 0 30\necho RAMP 1px/frame from x=100\npeekeach X FF8004 2\n"]
    s += [f"gun {100 + i} 100 0 1\n" for i in range(40)]
    s += ["gun 140 100 0 20\npeekeach off\n"]
    return "".join(s)


def report(outdir):
    pts, cur = [], None
    try:
        lines = open(f"{outdir}/peek.log").read().split("\n")
    except OSError:
        lines = []
    for l in lines:
        p = l.split()
        if len(p) < 3:
            continue
        if p[1] == "P":
            cur = {"x": int(p[2]), "y": int(p[3])}
            pts.append(cur)
        elif cur is not None and p[1] in ("X", "Y", "RAWY", "NLATCH"):
            cur[p[1]] = int(p[2], 16)
    pts = [q for q in pts if "X" in q]
    if pts:
        ex = collections.Counter(q["X"] - q["x"] for q in pts if q.get("NLATCH", 1))
        ey = collections.Counter(q["Y"] - q["y"] for q in pts if q.get("NLATCH", 1))
        print("points", len(pts), " no latch:", sum(1 for q in pts if q.get("NLATCH", 1) == 0))
        print("X error (game X - commanded x):", dict(sorted(ex.items())))
        print("Y error (game Y - commanded y, includes the 24..200 clamp):", dict(sorted(ey.items())))
        if "RAWY" in pts[0]:
            er = collections.Counter(q["RAWY"] - q["y"] for q in pts if q.get("NLATCH", 1))
            print("raw Y error ($FF9F70, before clamp):", dict(sorted(er.items())))
        ys = sorted({q["y"] for q in pts})
        if len(ys) > 1:
            xs = sorted({q["x"] for q in pts})
            print("X error map (rows = y, cols = x):")
            print("      " + "".join("%4d" % x for x in xs))
            for y in ys:
                row = {q["x"]: q for q in pts if q["y"] == y}
                print("%4d: " % y + "".join(
                    "   ." if row[x].get("NLATCH", 1) == 0 else "%4d" % (row[x]["X"] - x) for x in xs))
        else:
            got = {q["X"] for q in pts}
            miss = [X for X in range(2, 321, 2) if X not in got]
            print("game X values never produced:", miss)
    try:
        segs = collections.Counter()
        name = None
        marks = [l.split() for l in lines if len(l.split()) >= 2 and l.split()[1] in ("TAP1", "TAP2", "HOLD")]
        starts = [(int(m[0]), m[1]) for m in marks]
        for l in open(f"{outdir}/watch.log"):
            f, pc, w, a, v = l.split()
            if a == "ffa0a2" and v == "ff":
                name = [n for s, n in starts if s <= int(f)][-1] if starts else "?"
                segs[name] += 1
        if segs:
            print("shots per 600 frames:", dict(segs))
    except OSError:
        pass


if __name__ == "__main__":
    a = sys.argv[1:] or ["-h"]
    if a[0] == "grid":
        sys.stdout.write(grid(int(a[1]) if len(a) > 1 else 16))
    elif a[0] == "xsweep":
        sys.stdout.write(xsweep(int(a[1]) if len(a) > 1 else 100))
    elif a[0] == "tap":
        sys.stdout.write(tap())
    elif a[0] == "lag":
        sys.stdout.write(lag())
    elif a[0] == "report" and len(a) > 1:
        report(a[1])
    else:
        print(__doc__)
