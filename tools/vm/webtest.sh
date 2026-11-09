#!/bin/bash
# Build the browser version and run the headless browser test on a VM that has emsdk, node with
# puppeteer-core and chrome-headless-shell under /opt (emsdk in /opt/emsdk, node modules in
# /opt/webtest; tools/vm/web_provision.sh installs them). Tests the page three times: default (hle none), ?hle=all and ?hle=all&fps=on (high frame rate, docs/hifps.md).
# Run it on the VM: tools/vm/vmrun VM bash tools/vm/webtest.sh
set -e
cd "$(dirname "$0")/../.."
mkdir -p build
export EMSDK=/opt/emsdk
export PATH=/opt/webtest/node/bin:$PATH
export NODE_PATH=/opt/webtest/node_modules
export CHROME=$(find /opt/webtest/chrome -name chrome-headless-shell -type f | head -1)
tools/web_build.sh > build/web_build.log 2>&1 || { tail -30 build/web_build.log; exit 1; }
tail -n 4 build/web_build.log
rc=0
for q in "" "?hle=all" "?hle=all&fps=on"; do
  echo "== page query '${q}'"
  WEB_QUERY=$q tools/web_test.sh || rc=1
  grep -E '"(hle|hle_active|logic_fps|failed)"' runs/web/test.json
done
exit $rc
