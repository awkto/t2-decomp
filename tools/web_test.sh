#!/bin/bash
# Headless browser test of build/web (docs/web.md): ROM through the file input, click to play, boot,
# 1 PLAYER - USE MENACER, mouse aim and fire. Needs node with puppeteer-core (NODE_PATH),
# CHROME=<chrome or chrome-headless-shell>, and the ROM at rom/baserom.md.
# WEB_QUERY='?hle=all' tests the page with that URL query (default: none).
# Results: runs/web/test.json and PNGs (look at them: the game's crosshair must follow the mouse).
set -u
cd "$(dirname "$0")/.."
[ -n "${CHROME:-}" ] || { echo "web_test.sh: set CHROME" >&2; exit 2; }
[ -f build/web/index.html ] || { echo "web_test.sh: build/web missing (tools/web_build.sh)" >&2; exit 2; }
[ -f rom/baserom.md ] || { echo "web_test.sh: rom/baserom.md missing" >&2; exit 2; }
rm -rf runs/web
mkdir -p runs/web
(cd build/web && exec python3 -m http.server 8765 >/dev/null 2>&1) &
srv=$!
trap 'kill $srv 2>/dev/null' EXIT
sleep 1
if node tools/web_test.js "http://localhost:8765/${WEB_QUERY:-}" runs/web "$PWD/rom/baserom.md" > runs/web/test.json 2>&1; then
  echo "web: PASS"; rc=0
else
  echo "web: FAIL"; tail -30 runs/web/test.json; rc=1
fi
echo "t2.wasm $(stat -c %s build/web/t2.wasm) bytes ($(gzip -9 -c build/web/t2.wasm | wc -c) gzipped)," \
     "t2.js $(stat -c %s build/web/t2.js) bytes ($(gzip -9 -c build/web/t2.js | wc -c) gzipped)"
exit $rc
