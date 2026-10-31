#!/usr/bin/env python3
"""Scripts and checks for the mouse tests (port/tools/mousetest.sh, issue #5).

    mousecheck.py gengrid OUT.txt      port script: aim at a grid of points, log the crosshair
    mousecheck.py grid PEEK.LOG        check it: the shot must land within 2 px of the mouse
    mousecheck.py genlag OUT.txt       port script: jump the mouse, log the crosshair every frame
    mousecheck.py lag PEEK.LOG fast|game   frames the crosshair needs to reach the target
    mousecheck.py flow PEEK.LOG KIND   check the `echo step` lines of menuflow / twop / continue / missile

All of them read the peek log written by the port in script mode. Exit status 1 on a failed check.
"""
import re
import sys

XS = list(range(0, 320, 9)) + [319]
YS = list(range(24, 201, 22)) + [200]


def gengrid(out):
    L = ["load l1", "mouse 160 112", "run 20"]
    for y in YS:
        for x in XS:
            L += ["mouse %d %d" % (x, y), "run 4", "crosshair g_%d_%d" % (x, y)]
    open(out, "w").write("\n".join(L) + "\n")


def crosshair_lines(log):
    for ln in open(log):
        p = ln.split()
        if len(p) == 8 and p[1].startswith(("g_", "l_")):
            yield p[1], [int(v) for v in p[2:]], int(p[0])


def grid(log):
    ex, ey, es, bad = [], [], [], 0
    n = 0
    for name, v, _ in crosshair_lines(log):
        if not name.startswith("g_"):
            continue
        _, x, y = name.split("_")
        x, y = int(x), int(y)
        gx, gy, px, py, sx, sy = v
        n += 1
        ex.append(px - x)
        ey.append(py - y)
        es.append((sx - x, sy - y))
        if px != gx or py != gy:
            bad += 1
    if not n:
        print("FAIL: no samples")
        return 1
    h = lambda e: {k: e.count(k) for k in sorted(set(e))}
    print("%d points" % n)
    print("X error (p1_x - mouse x):   %s" % h(ex))
    print("Y error (p1_y - mouse y):   %s" % h(ey))
    print("sprite centre - mouse x:    %s" % h([a for a, _ in es]))
    print("sprite centre - mouse y:    %s" % h([b for _, b in es]))
    ok = max(abs(e) for e in ex) <= 2 and max(abs(e) for e in ey) <= 0 and bad == 0
    # the sprite table is rebuilt once a frame: allow a stray lagging sample (1 %)
    late = sum(1 for a, b in es if abs(a) > 2 or abs(b) > 1)
    print("sprite samples off by more than 2 px: %d of %d" % (late, len(es)))
    ok = ok and late * 100 <= len(es)
    within1 = sum(1 for e in ex if abs(e) <= 1) * 100.0 / len(ex)
    print("within 1 px in X: %.1f %%" % within1)
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


def genlag(out):
    L = ["load l1"]
    for i, (a, b) in enumerate([(40, 280), (280, 40), (60, 120), (120, 61)]):
        L += ["mouse %d 112" % a, "run 40", "mouse %d 112" % b]
        for f in range(1, 21):
            L += ["run 1", "crosshair l_%d_%d" % (i, f)]
    open(out, "w").write("\n".join(L) + "\n")


def lag(log, mode):
    runs = {}
    for name, v, _ in crosshair_lines(log):
        if name.startswith("l_"):
            _, i, f = name.split("_")
            runs.setdefault(int(i), {})[int(f)] = v[2]      # p1_x
    worst = 0
    for i, r in sorted(runs.items()):
        tgt = r[20]
        frames = next((f for f in sorted(r) if abs(r[f] - tgt) <= 2), 99)
        print("jump %d: crosshair x per frame %s -> settled at frame %d" % (i, [r[f] for f in range(1, 11)], frames))
        worst = max(worst, frames)
    print("worst %d frames (%s)" % (worst, mode))
    ok = worst <= 2 if mode == "fast" else 5 <= worst <= 16
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


def steps(log):
    """{step name: {label: value}} from the peek log: values logged after an `echo step NAME`."""
    d, cur = {}, None
    for ln in open(log):
        p = ln.split()
        if len(p) >= 3 and p[1] == "step":
            cur = p[2]
            d[cur] = {}
        elif cur and len(p) == 3 and p[1] == "screen":
            d[cur]["screen"] = p[2]
        elif cur and len(p) == 3 and re.fullmatch(r"[0-9a-f]+", p[2]):
            d[cur][p[1]] = int(p[2], 16)
        elif cur and len(p) == 3 and p[1] == "screen":
            d[cur]["screen"] = p[2]
    return d


EXPECT = {
    "menuflow": [
        ("menu-start", "cursor", 0), ("hover-row2", "cursor", 2), ("hover-row3", "cursor", 3),
        ("wheel-down-to-row4", "cursor", 4), ("wheel-up-to-row3", "cursor", 3), ("hover-row4", "cursor", 4),
        ("options-start", "opt_credits", 4), ("left-click-credits-up", "opt_credits", 5),
        ("two-right-clicks-credits-down", "opt_credits", 3), ("wheel-up", "opt_credits", 4),
        ("options-hover-row1", "cursor", 1), ("options-hover-exit", "cursor", 4),
        ("back-in-menu", "screen", "menu"), ("hover-row0", "cursor", 0),
        ("in-game", "mode", 4), ("in-game", "level", 1),
    ],
    "twop": [
        ("hover-row2", "cursor", 2), ("2p-in-game", "mode", 0), ("2p-in-game", "level", 1),
        ("2p-in-game", "p1_index", 0), ("2p-in-game", "p2_index", 1),
    ],
    "continue": [
        ("continue-screen", "screen", "continue"), ("after-left-click", "screen", "game"),
        ("hiscore-entry", "level", 0x0A),
    ],
    "missile": [],
}


def flow(log, kind):
    d = steps(log)
    ok = True
    for step, key, want in EXPECT[kind]:
        got = d.get(step, {}).get(key)
        good = got == want
        ok &= good
        print("%-4s %-32s %-12s got %s want %s" % ("ok" if good else "FAIL", step, key, got, want))
    extra = []
    if kind == "continue":
        a, b = d.get("continue-screen", {}), d.get("after-left-click", {})
        c0, c1 = a.get("credits"), b.get("credits")
        good = c0 is not None and c1 == c0 - 1
        ok &= good
        extra.append("%-4s credits %s -> %s after the left click (one credit used)" % ("ok" if good else "FAIL", c0, c1))
        good = b.get("energy", 0) > 0x8000
        ok &= good
        extra.append("%-4s energy refilled after the click: %s" % ("ok" if good else "FAIL", b.get("energy")))
        hs = None
        for ln in open(log):
            p = ln.split()
            if len(p) == 3 and p[1] == "hs1_initials":
                hs = bytes.fromhex(p[2])[:3]
        good = hs == b"ABC"
        ok &= good
        extra.append("%-4s high-score initials %s (want ABC)" % ("ok" if good else "FAIL", hs))
    if kind == "twop":
        v = {}
        for ln in open(log):
            p = ln.split()
            if len(p) == 3 and p[1] in ("p2x_a", "p2x_b"):
                v[p[1]] = int(p[2], 16)
        good = v.get("p2x_b", 0) > v.get("p2x_a", 0) + 20
        ok &= good
        extra.append("%-4s pad 1 moved player 2's crosshair: x %s -> %s" % ("ok" if good else "FAIL", v.get("p2x_a"), v.get("p2x_b")))
        cross = [(n, v_) for n, v_, _ in crosshair_lines_any(log)]
        for n, v_ in cross:
            if n == "p1_a":
                good = abs(v_[2] - 60) <= 2 and abs(v_[3] - 100) <= 0
                ok &= good
                extra.append("%-4s mouse aimed player 1: p1_x,p1_y = %d,%d (mouse 60,100)" % ("ok" if good else "FAIL", v_[2], v_[3]))
    if kind == "missile":
        v = {}
        for ln in open(log):
            p = ln.split()
            if len(p) == 3 and p[1] == "missiles":
                v.setdefault("m", []).append(int(p[2], 16))
        m = v.get("m", [])
        good = len(m) == 3 and m[0] == m[1] and m[2] == m[1] - 1
        ok &= good
        extra.append("%-4s missiles (BCD) %s: 1-frame tap does nothing, a 4-frame right click fires one" % ("ok" if good else "FAIL", m))
    for e in extra:
        print(e)
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


def crosshair_lines_any(log):
    for ln in open(log):
        p = ln.split()
        if len(p) == 8 and not p[1].startswith(("g_", "l_")) and p[1].startswith("p1_"):
            yield p[1], [int(v) for v in p[2:]], int(p[0])


def main():
    a = sys.argv[1:]
    if len(a) == 2 and a[0] == "gengrid":
        gengrid(a[1])
    elif len(a) == 2 and a[0] == "grid":
        return grid(a[1])
    elif len(a) == 2 and a[0] == "genlag":
        genlag(a[1])
    elif len(a) == 3 and a[0] == "lag":
        return lag(a[1], a[2])
    elif len(a) == 3 and a[0] == "flow":
        return flow(a[1], a[2])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
