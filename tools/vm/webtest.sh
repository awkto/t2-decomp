#!/bin/bash
# Build the browser version and run the headless browser test on a VM that has emsdk, node with
# puppeteer-core and chrome-headless-shell under /opt (emsdk in /opt/emsdk, node modules in
# /opt/webtest). Run it on the VM: tools/vm/vmrun VM bash tools/vm/webtest.sh
set -e
cd "$(dirname "$0")/../.."
mkdir -p build
export EMSDK=/opt/emsdk
export NODE_PATH=/opt/webtest/node_modules
export CHROME=$(find /opt/webtest/chrome -name chrome-headless-shell -type f | head -1)
tools/web_build.sh > build/web_build.log 2>&1 || { tail -30 build/web_build.log; exit 1; }
tail -n 4 build/web_build.log
tools/web_test.sh
