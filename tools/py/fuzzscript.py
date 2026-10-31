#!/usr/bin/env python3
"""Emit a harness script: boot, start a game from the title menu and play with random input.

    fuzzscript.py gun|pad|attract FRAMES SEED [--shots]

gun      Menacer game (title menu default item; harness env GUN=1): random aim + trigger/A/B buttons
pad      "USE JOYPAD 1" (second menu item): random pad presses
attract  no input, just run FRAMES frames from power-on (attract mode / demo)
"""
import argparse
import random


def gen(mode, frames, seed, shots=False):
    rng = random.Random(seed)
    out = ["run 1300"]
    if mode == "attract":
        out.append(f"run {frames}")
        return out
    if mode == "pad":
        out += ["press D 4", "run 20"]
    out += ["press S 4", "run 120"]
    if mode == "gun":
        for _ in range(6):
            out += ["gun 160 112 8 4", "run 60"]   # gun Start skips the intro text
    else:
        for _ in range(6):
            out += ["press S 4", "run 60"]
    t = 0
    nextshot = 600
    nextstart = 1500
    while t < frames:
        if mode == "gun":
            x, y = rng.randint(0, 319), rng.randint(0, 223)
            r = rng.random()
            btn = 1 if r < 0.7 else (2 if r < 0.8 else (4 if r < 0.9 else 0))
            n = rng.randint(2, 6)
            out.append(f"gun {x} {y} {btn} {n}")
            w = rng.randint(1, 12)
        else:
            b = rng.choice(["U", "D", "L", "R", "UL", "UR", "DL", "DR", "A", "B", "C", "A", "B", "C", "A", "B"])
            n = rng.randint(2, 20)
            out.append(f"press {b} {n}")
            w = rng.randint(1, 15)
        out.append(f"run {w}")
        t += n + w
        if t >= nextstart:   # keep going through game over / continue / title: press Start now and then
            out += (["gun 160 112 8 4", "run 30"] if mode == "gun" else ["press S 4", "run 30"]) * 2
            t += 68
            nextstart += rng.randint(1200, 2400)
        if shots and t >= nextshot:
            out.append(f"shot t{t}")
            nextshot += 600
    out.append("shot end")
    return out


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["gun", "pad", "attract"])
    ap.add_argument("frames", type=int)
    ap.add_argument("seed", type=int)
    ap.add_argument("--shots", action="store_true")
    a = ap.parse_args()
    print("\n".join(gen(a.mode, a.frames, a.seed, a.shots)))
