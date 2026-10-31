#!/bin/bash
# shot2png.sh DIR   convert every DIR/*.ppm screenshot to png (needs pnmtopng)
for f in "$1"/*.ppm; do pnmtopng "$f" > "${f%.ppm}.png" 2>/dev/null; done
