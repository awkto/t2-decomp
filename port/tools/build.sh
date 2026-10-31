#!/bin/sh
# Configure and build the native port into build/port (docs/port.md); prints only warnings and errors.
cd "$(dirname "$0")/../.." || exit 2
mkdir -p build
cmake -S port -B build/port -DCMAKE_BUILD_TYPE=Release > build/port.cmake.log 2>&1 || { cat build/port.cmake.log; exit 1; }
cmake --build build/port -j"${JOBS:-4}" 2>&1 | grep -E -B3 "warning|error|Error|undefined" | grep -v "romfind.c" | head -30
test -x build/port/t2port
