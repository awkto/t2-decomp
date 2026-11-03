#!/bin/bash
# One-time setup of a VM for tools/vm/webtest.sh: emsdk 6.0.13 in /opt/emsdk, node 18 with
# puppeteer-core 23 and chrome-headless-shell 127 in /opt/webtest. Idempotent.
# Run on the VM: tools/vm/vmrun VM bash tools/vm/web_provision.sh
set -euo pipefail
if [ ! -f /opt/emsdk/emsdk_env.sh ]; then
  git clone --depth 1 https://github.com/emscripten-core/emsdk.git /opt/emsdk
fi
(cd /opt/emsdk && ./emsdk install 6.0.13 && ./emsdk activate 6.0.13)
mkdir -p /opt/webtest
cd /opt/webtest
if [ ! -x node/bin/node ]; then
  curl -fsSL https://nodejs.org/dist/v18.20.8/node-v18.20.8-linux-x64.tar.xz | tar -xJ
  mv node-v18.20.8-linux-x64 node
fi
export PATH=/opt/webtest/node/bin:$PATH
[ -f package.json ] || npm init -y >/dev/null
npm ls puppeteer-core >/dev/null 2>&1 || npm install puppeteer-core@23
if [ -z "$(find chrome -name chrome-headless-shell -type f 2>/dev/null)" ]; then
  npx --yes @puppeteer/browsers install chrome-headless-shell@127 --path /opt/webtest/chrome
fi
find chrome -name chrome-headless-shell -type f
