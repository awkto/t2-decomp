#!/usr/bin/env python3
"""Lockstep gate: run --hle none and --hle SPEC from the same savestate with the same
scripted input for N frames and compare, every frame, the game's work RAM and
VRAM/CRAM/VSRAM/VDP registers.

    port/tools/lockstep.sh [options]                 # gate: --hle all from the default states
    port/tools/lockstep.sh --hle PalFade --frames 300
    port/tools/lockstep.sh --hle all --states "gun_01 gun_05" --addr

The comparison is done on the per-frame memory hashes t2port writes with --framehash
(port/src/emu.c, emu_frame_hash).  Two runs are deterministic, so the first line whose
memory hash differs is the first frame the two builds diverge.  With --addr the first
differing frame is reproduced (the input script truncated to exactly that many frames,
t2port --memdump) and the differing bytes are reported by region and address.

Exit status: 0 when every state is identical for the whole run, 1 on divergence/error.

This is the check every new C replacement must pass before it is trusted (docs/porting.md,
"The lockstep gate"): difftest proves one call equal, the gate proves the charges keep the
whole game in lockstep with the original.
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RUNS = os.path.join(ROOT, "runs", "lockstep")

# default gate states: gameplay of levels 1, 2, 4, 5, 7 and the title/menu
DEFAULT_STATES = ["gun_01", "gun_02", "gun_04", "gun_05", "gun_07", "title"]

# memory layout of --memdump (port/src/emu.c, emu_write_memdump)
REGIONS = [
    ("RAM", 0x00000, 0x10000, 0xFF0000),
    ("VRAM", 0x10000, 0x10000, 0x0000),
    ("CRAM", 0x20000, 0x80, 0x0000),
    ("VSRAM", 0x20080, 0x80, 0x0000),
    ("VDPREG", 0x20100, 0x20, 0x0000),
]


def gen_input(n):
    """Deterministic input covering mouse aim/clicks and pad buttons, exactly n frames."""
    lines, f, blk = [], 0, 0
    while f < n:
        x = 40 + (blk * 53) % 240
        y = 30 + (blk * 37) % 170
        cmds = [("mouse %d %d" % (x, y), 0), ("click L 6", 6), ("press - 18", 18),
                ("press A 6", 6), ("press U 6", 6), ("press D 6", 6),
                ("press B 6", 6), ("press C 6", 6), ("press - 6", 6)]
        for cmd, fr in cmds:
            if f >= n:
                break
            if fr == 0:
                lines.append(cmd)
                continue
            take = min(fr, n - f)
            p = cmd.split()
            if take == fr:
                lines.append(cmd)
            elif p[0] == "click":
                lines.append("click %s %d" % (p[1], take))
            else:                       # press BUTTONS k
                lines.append("press %s %d" % (p[1], take))
            f += take
        blk += 1
    return lines


def write_script(path, state, n):
    with open(path, "w") as f:
        f.write("load states/%s.state\n" % state)
        f.write("\n".join(gen_input(n)))
        f.write("\n")


def run(binary, script, outdir, hle, hashfile, env):
    os.makedirs(outdir, exist_ok=True)
    cmd = [binary, "--script", script, "--outdir", outdir, "--mute", "--no-vsync",
           "--hle", hle, "--framehash", hashfile]
    r = subprocess.run(cmd, cwd=ROOT, env=env, stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE, timeout=120)
    if r.returncode != 0:
        sys.stderr.write(r.stderr.decode(errors="replace"))
        raise RuntimeError("%s failed (hle=%s)" % (binary, hle))
    return hashfile


def read_hashes(path):
    """-> list of (frame, [live-RAM,vram,cram,vsram,vreg,all], cycles)"""
    out = []
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) < 9:
                continue
            out.append((int(p[0]), [p[7], p[2], p[3], p[4], p[5], p[6]], p[8]))
    return out


def first_diff(ref, got):
    """-> (frame, [region names that differ], ref_line, got_line) or None"""
    names = ["RAM", "VRAM", "CRAM", "VSRAM", "VDPREG", "ALL"]
    for i, (a, b) in enumerate(zip(ref, got)):
        if a[1][5] != b[1][5]:                     # combined hash
            diff = [names[j] for j in range(6) if a[1][j] != b[1][j]]
            return a[0], diff, a, b
    if len(ref) != len(got):
        return (ref[-1][0] if ref else 0), ["length"], (ref[-1] if ref else None), (got[-1] if got else None)
    return None


def read_memdump(path):
    with open(path, "rb") as f:
        return f.read()


def report_addr(ref_path, got_path, maxrep=16):
    a, b = read_memdump(ref_path), read_memdump(got_path)
    n = min(len(a), len(b))
    for name, off, size, base in REGIONS:
        if name == "RAM":
            size = 0xFC00                 # exclude the system stack (see emu.c T2_LIVE_RAM_END)
        diffs = [(i, a[i], b[i]) for i in range(off, min(off + size, n))
                 if a[i] != b[i]]
        if diffs:
            print("    %s: %d byte(s) differ; first %d:" % (name, len(diffs), min(maxrep, len(diffs))))
            for ad, va, vb in diffs[:maxrep]:
                print("      $%06X: none=%02X spec=%02X" % (base + (ad - off), va, vb))


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--frames", type=int, default=3000)
    ap.add_argument("--states", default=" ".join(DEFAULT_STATES),
                    help="space separated state names under states/ (default: %s)" % " ".join(DEFAULT_STATES))
    ap.add_argument("--hle", default="all", help="the spec compared against --hle none (default all)")
    ap.add_argument("--bin", default=os.path.join(ROOT, "build", "port", "t2port"))
    ap.add_argument("--addr", action="store_true", help="on the first differing frame, dump and diff memories")
    ap.add_argument("--keep", action="store_true", help="keep runs/lockstep/<state> outputs")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--z80", action="store_true", help="include Z80 RAM (the sound state) in the compared hash")
    a = ap.parse_args()

    if not os.path.exists(a.bin):
        sys.stderr.write("%s: not built (run port/tools/build.sh)\n" % a.bin)
        return 2
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    if a.z80:
        env["T2_HASH_Z80"] = "1"
    states = a.states.split()
    os.makedirs(RUNS, exist_ok=True)

    bad = 0
    for st in states:
        d = os.path.join(RUNS, st)
        os.makedirs(d, exist_ok=True)
        script = os.path.join(d, "script.txt")
        write_script(script, st, a.frames)
        ref_h = run(a.bin, script, d, "none", os.path.join(d, "none.hash"), env)
        got_h = run(a.bin, script, d, a.hle, os.path.join(d, "spec.hash"), env)
        ref, got = read_hashes(ref_h), read_hashes(got_h)
        fd = first_diff(ref, got)
        if fd is None:
            print("%-10s %d frames identical" % (st, len(ref)))
            continue
        bad += 1
        frame, regions, rl, gl = fd
        print("%-10s DIVERGES at frame %d (%s)%s" % (st, frame, ",".join(regions),
              "  [cycle none=%s spec=%s]" % (rl[2], gl[2]) if rl and gl else ""))
        if a.addr and frame:
            # reproduce exactly `frame` frames and dump the memories at exit
            script_t = os.path.join(d, "script_trunc.txt")
            write_script(script_t, st, frame)
            rp = os.path.join(d, "none.mem")
            gp = os.path.join(d, "spec.mem")
            # run with --memdump (no framehash needed)
            for hle, out in (("none", rp), (a.hle, gp)):
                subprocess.run([a.bin, "--script", script_t, "--outdir", d, "--mute",
                                "--no-vsync", "--hle", hle, "--memdump", out],
                               cwd=ROOT, env=env, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, timeout=120)
            report_addr(rp, gp)
    if not a.quiet:
        print("gate: %s -> %s" % (a.hle, "OK" if not bad else "%d state(s) diverge" % bad))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
