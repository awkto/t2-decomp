"""Per-project constants shared by the tools (the only place that names the game)."""
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
NAME = "T2: The Arcade Game (Genesis, JUE)"
SRC_NAME = "t2"                      # src/<SRC_NAME>.s, build/<SRC_NAME>.md
SHA1 = "85cc1cf3379d3ce23ca3c03d84fe6e2b3adc9c56"
ROM_SIZE = 0x100000
# 68k code area: data/code_regions.txt, one "start end" hex pair per line (end exclusive, '#' comments).
# Bounds the gap-fill heuristic and indirect-target checks; edit when coverage shows code elsewhere.


def code_regions():
    out = []
    for line in open(os.path.join(ROOT, "data", "code_regions.txt")):
        line = line.split("#", 1)[0].split()
        if line:
            out.append((int(line[0], 16), int(line[1], 16)))
    return out
