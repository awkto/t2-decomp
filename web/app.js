// Page shell of the browser build (docs/web.md).
// Mounts IndexedDB at /t2 (IDBFS), finds the ROM, starts the C program (port/src/web/main_web.c).
// ROM order: the copy kept in IndexedDB, then the player's own file (picker / drag and drop). The ROM is checked by sha1 (in C) and never
// leaves the browser.
'use strict';

const ROM_SHA1 = '85cc1cf3379d3ce23ca3c03d84fe6e2b3adc9c56';
const ROM_DIR = '/t2/rom';
const ROM_CACHED = ROM_DIR + '/baserom.md';
const $ = id => document.getElementById(id);

window.t2State = 'loading';           // loading | need-rom | running | error (for tests)
const errLines = [];

function setStatus(t) { $('status').textContent = t; }

function showFatal(title, text) {
  window.t2State = 'error';
  $('rom-panel').hidden = true;
  $('error-title').textContent = title;
  $('error-text').textContent = text;
  $('error-log').textContent = errLines.join('\n');
  $('error-panel').hidden = false;
  setStatus('stopped');
}

// a fatal error of the C program (abort or a trap)
function onAbort(what) { showFatal('The game stopped', String(what || 'unknown error')); }

let syncTimer = 0, syncing = false, syncAgain = false;
function syncNow() {
  if (syncing) { syncAgain = true; return; }
  syncing = true;
  Module.FS.syncfs(false, err => {
    syncing = false;
    if (err) console.warn('IndexedDB sync failed', err);
    if (syncAgain) { syncAgain = false; syncNow(); }
  });
}

var Module = window.Module = {
  canvas: $('canvas'),
  noInitialRun: true,
  print: t => console.log(t),
  printErr: t => {
    console.warn(t);
    errLines.push(t);
    if (errLines.length > 60) errLines.shift();
  },
  onAbort,
  setStatus: t => { if (t && window.t2State === 'loading') setStatus(t); },
  preRun: [() => {
    const FS = Module.FS;
    FS.mkdir('/t2');
    FS.mount(Module.IDBFS || FS.filesystems.IDBFS, {}, '/t2');
    Module.addRunDependency('idbfs');
    FS.syncfs(true, err => {
      if (err) console.warn('IndexedDB unavailable: saves will not be kept', err);
      Module.removeRunDependency('idbfs');
    });
  }],
  onRuntimeInitialized: () => { loadRightSetting(); loadWideSetting(); findRom(); },
  // called from C
  t2Sync: () => { clearTimeout(syncTimer); syncTimer = setTimeout(syncNow, 300); },
  t2Status: t => setStatus(t),
  t2Started: () => {
    window.t2State = 'running';
    if (pendingRight) Module.ccall('web_set_option', 'number', ['string', 'string'], ['mouse_right', pendingRight]);
    if (pendingWide) Module.ccall('web_set_option', 'number', ['string', 'string'], ['widescreen', pendingWide]);
    if (pendingFps) Module.ccall('web_set_option', 'number', ['string', 'string'], ['fps', pendingFps]);
  },
  t2Fatal: t => showFatal('Cannot start the game', t),
};

function exists(path) {
  try { Module.FS.stat(path); return true; } catch (e) { return false; }
}

function romOk(path) {
  return Module.ccall('web_rom_check', 'number', ['string'], [path]) === 1;
}

// Browsers only let a page make sound after a user gesture. A ROM kept from an earlier visit would
// start the game without one, so the page waits for "Click to play" first: SDL creates its
// AudioContext inside callMain, so creating it in the click handler means it starts 'running'
// instead of silently 'suspended'. A ROM the player has just chosen or dropped also goes through it, so
// the Mode menu on the panel can be set first.
function clickToPlay(path, source = 'cached') {
  $('rom-panel').hidden = true;
  $('play-panel').hidden = false;
  setStatus('ready');
  const go = () => { $('play-panel').hidden = true; startNow(path, source); resumeAudio(); };
  $('btn-play').addEventListener('click', go, { once: true });
}

function startNow(path, source) {
  $('rom-panel').hidden = true;
  $('btn-forget').hidden = source !== 'cached';
  setStatus('starting (' + source + ' ROM)');
  console.log('t2: starting with the ' + source + ' ROM');
  $('canvas').focus();
  try {
    // ?hle=all|none|NAME+NAME overrides the ini's hle option for this visit (docs/web.md)
    // ?fps=off|on|120 (docs/hifps.md) and ?wide=off|16:9|21:9|W:H|WIDTH (docs/widescreen.md) override the ini's options for
    // this visit; they go to main as key=value arguments after the ROM path
    const q = new URLSearchParams(location.search);
    // the play panel's Mode menu: '' = the ini's hle option (default none, the emulated 68k), all, native
    const mode = $('sel-mode').value;
    if (mode === 'native') Module.ccall('web_set_native', 'number', ['number'], [1]);
    const hle = q.get("hle") || (mode ? 'all' : null), fps = q.get("fps"), wide = q.get("wide");
    console.log('t2: mode ' + (mode || 'emulated') + (q.get("hle") ? ' (hle from the URL: ' + q.get("hle") + ')' : ''));
    const args = [path];
    if (hle) args.push('hle=' + hle);
    if (fps) args.push('fps=' + fps);
    if (wide) args.push('wide=' + wide);
    Module.callMain(args);
  } catch (e) {
    if (e !== 'unwind' && !(e && e.name === 'ExitStatus')) throw e;
  }
}

async function findRom() {
  const FS = Module.FS;
  $('rom-sha1').textContent = ROM_SHA1;
  // 1. the copy kept in IndexedDB
  if (exists(ROM_CACHED)) {
    if (romOk(ROM_CACHED)) return clickToPlay(ROM_CACHED);
    FS.unlink(ROM_CACHED);
  }
  // 2. a ROM served next to the page (private deployments only; the public build has none)
  try {
    const r = await fetch('baserom.md', { cache: 'no-cache' });
    if (r.ok) {
      const buf = new Uint8Array(await r.arrayBuffer());
      const tmp = '/tmp/bundled.md';          // MEMFS, not kept in IndexedDB
      FS.writeFile(tmp, buf);
      if (romOk(tmp)) return clickToPlay(tmp, 'bundled');
      FS.unlink(tmp);
    }
  } catch (e) { /* no bundled ROM */ }
  // 3. ask
  window.t2State = 'need-rom';
  setStatus('waiting for the ROM');
  $('rom-panel').hidden = false;
}

async function takeRomFile(file) {
  const FS = Module.FS;
  const err = $('rom-error');
  err.hidden = true;
  if (!file) return;
  if (file.size < 0x1000 || file.size > 8 * 1024 * 1024) {
    err.textContent = file.name + ' is not a Genesis ROM (size ' + file.size + ' bytes).';
    err.hidden = false;
    return;
  }
  const buf = new Uint8Array(await file.arrayBuffer());
  if (!exists(ROM_DIR)) FS.mkdir(ROM_DIR);
  const tmp = ROM_DIR + '/incoming.md';
  FS.writeFile(tmp, buf);
  if (!romOk(tmp)) {
    FS.unlink(tmp);
    err.textContent = file.name + ' is not the right ROM: this needs T2: The Arcade Game (JUE), sha1 ' + ROM_SHA1 + '.';
    err.hidden = false;
    return;
  }
  FS.rename(tmp, ROM_CACHED);
  syncNow();
  clickToPlay(ROM_CACHED);
}

$('rom-file').addEventListener('change', e => takeRomFile(e.target.files[0]));
const drop = $('drop');
for (const t of ['dragenter', 'dragover']) drop.addEventListener(t, e => { e.preventDefault(); drop.classList.add('over'); });
for (const t of ['dragleave', 'drop']) drop.addEventListener(t, e => { e.preventDefault(); drop.classList.remove('over'); });
drop.addEventListener('drop', e => takeRomFile(e.dataTransfer.files[0]));
// a file dropped anywhere on the page while it asks for the ROM
document.addEventListener('dragover', e => e.preventDefault());
document.addEventListener('drop', e => {
  e.preventDefault();
  if (window.t2State === 'need-rom' && e.dataTransfer.files[0]) takeRomFile(e.dataTransfer.files[0]);
});

$('btn-forget').addEventListener('click', () => {
  if (!confirm('Remove the ROM stored in this browser? The page reloads.')) return;
  try { Module.FS.unlink(ROM_CACHED); } catch (e) { /* already gone */ }
  Module.FS.syncfs(false, () => location.reload());
});
$('btn-reload').addEventListener('click', () => location.reload());

// fullscreen: the stage (canvas) only; F11 does the same
function toggleFullscreen() {
  if (document.fullscreenElement) document.exitFullscreen();
  else $('stage').requestFullscreen().catch(() => {});
  $('canvas').focus();
}
$('btn-fullscreen').addEventListener('click', toggleFullscreen);

// the game's keys must not scroll the page or trigger browser shortcuts (F5 reload, F3 find...)
const GAME_KEYS = new Set(['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Tab', 'Enter', ' ',
  'F1', 'F2', 'F3', 'F4', 'F5', 'F6', 'F7', 'F8', 'F9', 'F10']);
window.addEventListener('keydown', e => {
  if (window.t2State !== 'running') return;
  if (e.key === 'F11') { e.preventDefault(); toggleFullscreen(); return; }
  if (GAME_KEYS.has(e.key)) e.preventDefault();
}, true);

// sound: browsers keep an AudioContext suspended until a user gesture
function audioCtx() { return Module.SDL2 && Module.SDL2.audioContext; }
function resumeAudio() {
  const ctx = audioCtx();
  if (ctx && ctx.state !== 'running') ctx.resume().catch(() => {});
}
for (const t of ['pointerdown', 'keydown', 'touchstart']) document.addEventListener(t, resumeAudio, true);
$('btn-sound').addEventListener('click', resumeAudio);
setInterval(() => {
  const ctx = audioCtx();
  $('btn-sound').hidden = !(ctx && ctx.state !== 'running' && window.t2State === 'running');
}, 500);

// keep IndexedDB current when the tab goes away
document.addEventListener('visibilitychange', () => { if (document.hidden && Module.FS) syncNow(); });

window.addEventListener('error', e => {
  if (window.t2State === 'running' || window.t2State === 'loading')
    errLines.push(String(e.message || e.error));
});

// right mouse button = Menacer A, B or C (kept in the ini by the C side)
const selRight = $('sel-right');
selRight.addEventListener('change', () => {
  if (window.t2State === 'running') Module.ccall('web_set_option', 'number', ['string', 'string'], ['mouse_right', selRight.value]);
  else pendingRight = selRight.value;
  $('canvas').focus();
});
let pendingRight = null;

// widescreen: kept in the ini by the C side; read once at start, so a change reloads the page. ?wide= wins.
const selWide = $('sel-wide');
selWide.addEventListener('change', () => {
  const p = new URLSearchParams(location.search);
  p.delete('wide');
  const q = p.toString();
  history.replaceState(null, '', location.pathname + (q ? '?' + q : '') + location.hash);
  if (window.t2State === 'running') {
    Module.ccall('web_set_option', 'number', ['string', 'string'], ['widescreen', selWide.value]);
    setStatus('widescreen ' + selWide.value + ': reloading');
    setTimeout(() => location.reload(), 900);
  } else pendingWide = selWide.value;
});
let pendingWide = null;
function loadWideSetting() {
  const q = new URLSearchParams(location.search).get('wide');
  if (q) { if ([...selWide.options].some(o => o.value === q)) selWide.value = q; return; }
  try {
    const m = /^widescreen\s*=\s*(\S+)/mi.exec(Module.FS.readFile('/t2/t2.ini', { encoding: 'utf8' }));
    if (m && [...selWide.options].some(o => o.value === m[1])) selWide.value = m[1];
  } catch (e) { /* no ini yet */ }
}
// frame rate: 60 Hz, or interpolated pictures on every display refresh (kept in the ini by the C side)
const selFps = $('sel-fps');
selFps.addEventListener('change', () => {
  if (window.t2State === 'running') Module.ccall('web_set_option', 'number', ['string', 'string'], ['fps', selFps.value]);
  else pendingFps = selFps.value;
  $('canvas').focus();
});
let pendingFps = null;

// Mode menu on the play panel: remembered in this browser only
const selMode = $('sel-mode');
try { const m = localStorage.getItem('t2-mode'); if (m !== null && [...selMode.options].some(o => o.value === m)) selMode.value = m; } catch (e) { /* no storage */ }
selMode.addEventListener('change', () => { try { localStorage.setItem('t2-mode', selMode.value); } catch (e) { /* no storage */ } });
function loadRightSetting() {
  try {
    const ini = Module.FS.readFile('/t2/t2.ini', { encoding: 'utf8' });
    const m = /^mouse_right\s*=\s*([ABC])/mi.exec(ini);
    if (m) selRight.value = m[1].toUpperCase();
    const f = /^fps\s*=\s*(\S+)/mi.exec(ini);
    if (f) selFps.value = /^(off|0)$/i.test(f[1]) ? 'off' : 'on';
    const qf = new URLSearchParams(location.search).get("fps");
    if (qf) selFps.value = /^(off|0)$/i.test(qf) ? 'off' : 'on';
  } catch (e) { /* no ini yet */ }
}

// the middle button is Start: no browser autoscroll; the right button must not open a menu
$('canvas').addEventListener('mousedown', e => { if (e.button === 1) e.preventDefault(); });
$('canvas').addEventListener('auxclick', e => e.preventDefault());
$('canvas').addEventListener('contextmenu', e => e.preventDefault());
