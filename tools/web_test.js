// Headless browser test of the web build (docs/web.md).
//   node tools/web_test.js URL OUTDIR ROM
// Fresh profile: the page must ask for the ROM; the ROM is given through the file input; the page
// shows "Click to play"; the game boots to the title menu, "1 PLAYER - USE MENACER" is started, the
// intro is skipped with the middle button, then the mouse is moved over the canvas to several points
// (game pixels, placed with the same letterbox math as the host) and clicked. After each move the
// gun position the C side holds is compared with the commanded pixel, and screenshots of the stage
// are written for a look at the game's own crosshair. Then: right button -> Menacer B, pointer off
// the picture -> gun off screen, a touch tap, and a reload that starts from the cached ROM.
// Needs puppeteer-core (NODE_PATH) and CHROME=<chrome or chrome-headless-shell binary>.
// Writes OUTDIR/*.png and prints a JSON summary; console errors fail the run.
'use strict';
const puppeteer = require('puppeteer-core');
const fs = require('fs');
const path = require('path');

const [url, outdir, rom] = process.argv.slice(2);
if (!url || !outdir || !rom) { console.log('usage: web_test.js URL OUTDIR ROM'); process.exit(2); }
fs.mkdirSync(outdir, { recursive: true });
const errors = [], warnings = [], logs = [];
const res = { checks: {} };
let gpage = null;
const watchdog = setTimeout(async () => {
  try { res.status_at_timeout = await gpage.evaluate(() => document.getElementById('status').textContent + ' frame ' + Module._web_frame_count()); } catch (e) { /* page gone */ }
  res.errors = errors; res.logs = logs.slice(-20);
  console.log(JSON.stringify(res, null, 1));
  console.log('WATCHDOG: timed out');
  process.exit(3);
}, 300000);
const sleep = ms => new Promise(r => setTimeout(r, ms));
const check = (name, ok, info) => { res.checks[name] = ok ? 'ok' : 'FAIL ' + JSON.stringify(info); };

(async () => {
  const browser = await puppeteer.launch({
    executablePath: process.env.CHROME,
    headless: true,
    args: ['--no-sandbox', '--disable-dev-shm-usage', '--enable-unsafe-swiftshader', '--use-angle=swiftshader', '--window-size=1280,1000'],
  });
  const page = await browser.newPage();
  gpage = page;
  await page.setViewport({ width: 1280, height: 1000, deviceScaleFactor: Number(process.env.DPR || 1), hasTouch: true });

  page.on('console', m => {
    const t = m.text();
    if (m.type() === 'error') errors.push(t);
    else if (m.type() === 'warning') warnings.push(t);
    else logs.push(t);
  });
  page.on('pageerror', e => errors.push('pageerror: ' + e.message));
  page.on('requestfailed', r => errors.push('requestfailed: ' + r.url()));

  const state = () => page.evaluate(() => window.t2State);
  const frame = () => page.evaluate(() => Module._web_frame_count());
  const gun = () => page.evaluate(() => [0, 1, 2, 3].map(i => Module._web_gun_state(i)));
  const waitState = async (want, ms) => {
    await page.waitForFunction(w => window.t2State === w || window.t2State === 'error', { timeout: ms }, want);
    const s = await state();
    if (s !== want) throw new Error('state ' + s + ': ' + await page.evaluate(() => document.getElementById('error-text').textContent + '\n' + document.getElementById('error-log').textContent));
  };
  const waitFrame = n => page.waitForFunction(n => Module._web_frame_count() >= n, { timeout: 120000, polling: 'raf' }, n);
  const shot = async name => {
    const p = path.join(outdir, name + '.png');
    await (await page.$('#stage')).screenshot({ path: p });
    return p;
  };
  // fast forward (Tab held = 4x) while waiting for a frame count
  const ffTo = async n => {
    await page.keyboard.down('Tab');
    await waitFrame(n);
    await page.keyboard.up('Tab');
  };
  const tap = async key => {
    const f = await frame();
    await page.keyboard.down(key); await waitFrame(f + 4); await page.keyboard.up(key);
  };

  const t0 = Date.now();
  await page.goto(url, { waitUntil: 'load' });
  await waitState('need-rom', 30000);
  res.need_rom_ms = Date.now() - t0;
  const input = await page.$('#rom-file');
  await input.uploadFile(rom);
  // a ROM the player has just chosen starts the game (the choice was the gesture) ...
  await waitState('running', 30000);
  check('ROM from the file input starts the game', true);
  await sleep(1000);                            // the debounced IndexedDB sync
  // ... a later visit finds it in IndexedDB and waits for "Click to play" (the AudioContext gate)
  await page.reload({ waitUntil: 'load' });
  await page.waitForFunction(() => !document.getElementById('play-panel').hidden, { timeout: 30000 });
  check('play gate shown on a later visit', true);
  check('game not started before the click', (await state()) !== 'running', await state());
  res.shots = [await shot('00-play-gate')];
  await page.click('#btn-play');
  await waitState('running', 30000);
  res.running_ms = Date.now() - t0;
  res.audio = await page.evaluate(() => Module.SDL2 && Module.SDL2.audioContext ? Module.SDL2.audioContext.state : 'none');

  // boot to the title menu, start "1 PLAYER - USE MENACER" (the default entry), run the intro
  await ffTo(1300);
  res.shots.push(await shot('01-title'));
  for (let i = 0; i < 6; i++) { await tap('Enter'); const f = await frame(); await ffTo(f + 90); }
  res.shots.push(await shot('02-intro'));

  // geometry of the canvas, with the host's integer-scale letterbox
  const g = await page.evaluate(() => {
    const r = document.getElementById('canvas').getBoundingClientRect();
    return { x: r.x, y: r.y, w: r.width, h: r.height };
  });
  const s = Math.min(Math.floor(g.w / 320), Math.floor(g.h / 224));
  const x0 = Math.floor((g.w - 320 * s) / 2), y0 = Math.floor((g.h - 224 * s) / 2);
  res.canvas = { css: g, scale: s, x0, y0 };
  const at = (gx, gy) => [g.x + x0 + (gx + 0.5) * s, g.y + y0 + (gy + 0.5) * s];
  const moveTo = async (gx, gy) => { const [x, y] = at(gx, gy); await page.mouse.move(x, y); await sleep(120); };

  // the gun's Start (middle button) skips the intro text
  await moveTo(160, 112);
  await page.mouse.click(...at(160, 112), { button: 'middle' });
  let f = await frame();
  await ffTo(f + 1500);
  res.shots.push(await shot('03-play'));

  // aim: the crosshair must follow the mouse
  const pts = [[40, 60], [280, 180], [80, 160], [240, 60], [160, 112], [0, 0], [319, 223]];
  res.aim = [];
  let k = 4;
  for (const [gx, gy] of pts) {
    await moveTo(gx, gy);
    f = await frame(); await waitFrame(f + 8);
    const st = await gun();
    res.aim.push({ want: [gx, gy], got: st.slice(0, 2), off: st[2] });
    check('aim ' + gx + ',' + gy, st[0] === gx && st[1] === gy && st[2] === 0, st);
    if (k-- > 0) res.shots.push(await shot('04-aim-' + gx + '-' + gy));
  }
  // off the picture: in the letterbox margin
  await page.mouse.move(g.x + 3, g.y + 3);
  f = await frame(); await waitFrame(f + 8);
  const off = await gun();
  check('off picture -> gun off screen', off[2] === 1, off);
  res.shots.push(await shot('05-off'));

  // fire: left click = trigger (A, mask 1); sample the mask while the button is down
  const fired = [];
  for (const [gx, gy] of [[160, 100], [180, 130], [100, 130]]) {
    await moveTo(gx, gy);
    await page.mouse.down();
    await sleep(100);
    fired.push((await gun())[3]);
    await page.mouse.up();
    f = await frame(); await waitFrame(f + 15);
    res.shots.push(await shot('06-fire-' + gx + '-' + gy));
  }
  res.fired_masks = fired;
  check('left button = trigger A', fired.every(m => m === 1), fired);
  // a click shorter than a frame must still be seen
  await moveTo(230, 130);
  const f1 = await frame();
  await page.mouse.click(...at(230, 130));
  await waitFrame(f1 + 2);
  // right button = B (mask 2) and no context menu
  await page.evaluate(() => { window.__ctx = []; document.getElementById('canvas').addEventListener('contextmenu', e => window.__ctx.push(e.defaultPrevented)); });
  await page.mouse.down({ button: 'right' }); await sleep(100);
  const right = (await gun())[3];
  await page.mouse.up({ button: 'right' });
  check('right button = Menacer B', right === 2, right);
  check('context menu suppressed', (await page.evaluate(() => window.__ctx)).every(x => x), await page.evaluate(() => window.__ctx));
  await page.mouse.down({ button: 'middle' }); await sleep(100);
  const mid = (await gun())[3];
  await page.mouse.up({ button: 'middle' });
  check('middle button = Start', mid === 8, mid);
  // the CSS cursor over the canvas is hidden
  check('cursor hidden', (await page.evaluate(() => getComputedStyle(document.getElementById('canvas')).cursor)) === 'none');
  // the right-button setting
  await page.select('#sel-right', 'C');
  await sleep(200);
  await page.mouse.down({ button: 'right' }); await sleep(100);
  const rightC = (await gun())[3];
  await page.mouse.up({ button: 'right' });
  check('right button = Menacer C after the setting', rightC === 4, rightC);
  await page.select('#sel-right', 'B');

  // touch: a tap aims and fires
  const [tx, ty] = at(120, 90);
  await page.touchscreen.tap(tx, ty);
  await sleep(200);
  const tg = await gun();
  res.touch_gun = tg;
  check('touch tap aims', Math.abs(tg[0] - 120) <= 1 && Math.abs(tg[1] - 90) <= 1, tg);
  res.shots.push(await shot('07-touch'));

  // fps
  const f0 = await frame(), tA = Date.now();
  await sleep(4000);
  res.logic_fps = Math.round((await frame() - f0) * 1000 / (Date.now() - tA) * 10) / 10;
  res.status = await page.evaluate(() => document.getElementById('status').textContent);

  check('ROM kept in IndexedDB (Forget ROM offered)', await page.evaluate(() => !document.getElementById('btn-forget').hidden));
  check('right-button setting is B again', (await page.evaluate(() => document.getElementById('sel-right').value)) === 'B');

  // ---- the menu flow by mouse alone: reload (a fresh game), no key is pressed from here on ----
  await page.reload({ waitUntil: 'load' });
  await page.waitForFunction(() => !document.getElementById('play-panel').hidden, { timeout: 30000 });
  await page.click('#btn-play');
  await waitState('running', 30000);
  const SCR = { game: 0, logo: 1, title: 2, menu: 3, options: 4, continue: 5, intro: 6 };
  const screen = () => page.evaluate(() => Module._web_screen());
  const ram = a => page.evaluate(a => Module._web_ram16(a), a);
  // fast forward (Tab = 4x; the only key, and it is not a game key) until pred() holds
  const ffUntil = async (pred, what, ms = 120000) => {
    const t = Date.now();
    await page.keyboard.down('Tab');
    try {
      while (!(await pred())) {
        if (Date.now() - t > ms) throw new Error('timeout waiting for ' + what);
        await sleep(40);
      }
    } finally { await page.keyboard.up('Tab'); }
  };
  const g2 = await page.evaluate(() => { const r = document.getElementById('canvas').getBoundingClientRect(); return { x: r.x, y: r.y, w: r.width, h: r.height }; });
  const s2 = Math.min(Math.floor(g2.w / 320), Math.floor(g2.h / 224));
  const at2 = (gx, gy) => [g2.x + Math.floor((g2.w - 320 * s2) / 2) + (gx + 0.5) * s2, g2.y + Math.floor((g2.h - 224 * s2) / 2) + (gy + 0.5) * s2];
  const mv = async (gx, gy) => { await page.mouse.move(...at2(gx, gy)); await sleep(100); };
  const clickAt = async (gx, gy, button = 'left') => { await mv(gx, gy); await page.mouse.click(...at2(gx, gy), { button }); await sleep(100); };
  await mv(160, 112);
  await ffUntil(async () => (await screen()) === SCR.logo, 'the logo screen');
  await page.mouse.click(...at2(160, 112));                   // Start waits for the logo's window
  await ffUntil(async () => (await screen()) === SCR.intro, 'the story text');
  await page.mouse.click(...at2(160, 112));
  await ffUntil(async () => (await screen()) === SCR.title, 'PRESS START');
  res.shots.push(await shot('10-mouse-title'));
  await page.mouse.click(...at2(160, 112));
  await ffUntil(async () => (await screen()) === SCR.menu, 'the main menu');
  await sleep(300);
  res.shots.push(await shot('11-mouse-menu'));
  check('mouse only: logos, story, PRESS START -> main menu', true);
  await mv(100, 92);                                          // row 2
  await ffUntil(async () => (await ram(0xFF9F60)) === 2, 'cursor on row 2', 20000);
  check('hover moves the menu cursor to row 2', (await ram(0xFF9F60)) === 2, await ram(0xFF9F60));
  res.shots.push(await shot('12-mouse-hover-row2'));
  await page.mouse.wheel({ deltaY: -100 });                   // wheel up: one row up
  await ffUntil(async () => (await ram(0xFF9F60)) === 1, 'wheel up', 20000);
  check('wheel up moves the cursor up', (await ram(0xFF9F60)) === 1, await ram(0xFF9F60));
  await clickAt(100, 150);                                    // OPTIONS (row 4)
  await ffUntil(async () => (await screen()) === SCR.options, 'OPTIONS', 20000);
  await sleep(300);
  const c0 = await ram(0xFFAE84);
  await clickAt(100, 44);                                     // credits row: left click = up
  await ffUntil(async () => (await ram(0xFFAE84)) !== c0, 'credits up', 20000);
  const c1 = await ram(0xFFAE84);
  check('OPTIONS: left click raises credits', c1 === c0 + 1, [c0, c1]);
  await page.mouse.click(...at2(100, 44), { button: 'right' });
  await ffUntil(async () => (await ram(0xFFAE84)) === c0, 'credits down', 20000);
  check('OPTIONS: right click lowers credits', (await ram(0xFFAE84)) === c0, await ram(0xFFAE84));
  res.shots.push(await shot('13-mouse-options'));
  await clickAt(100, 140);                                    // EXIT
  await ffUntil(async () => (await screen()) === SCR.menu, 'back in the menu', 20000);
  await sleep(300);
  await clickAt(100, 44);                                     // START 1 PLAYER - USE MENACER
  await ffUntil(async () => (await ram(0xFF80CC)) === 1, 'level 1', 60000);
  check('mouse only: menu -> level 1', (await screen()) === SCR.game, await screen());
  check('mouse only: Menacer mode 4', ((await ram(0xFF9DF4)) & 0xFF) === 4, await ram(0xFF9DF4));
  f = await frame(); await ffTo(f + 600);
  res.shots.push(await shot('14-mouse-level1'));
  // the shot lands where the mouse is, to within 2 px (aim_correct), a few frames after the move
  for (const gx of [30, 101, 177, 250, 301]) {
    await moveTo(gx, 100);
    f = await frame(); await waitFrame(f + 4);
    const px = await ram(0xFF8004);
    check('aim_correct: p1_x for mouse x ' + gx, Math.abs(px - gx) <= 2, px);
  }

  res.errors = errors;
  res.warnings = warnings.slice(0, 20);
  res.logs = logs.slice(0, 20);
  const bad = Object.entries(res.checks).filter(([, v]) => v !== 'ok');
  res.failed = bad.length;
  console.log(JSON.stringify(res, null, 1));
  clearTimeout(watchdog);
  await browser.close();
  process.exit(errors.length || bad.length ? 1 : 0);
})().catch(e => {
  res.errors = errors; res.warnings = warnings.slice(-30); res.logs = logs.slice(-30);
  console.log(JSON.stringify(res, null, 1));
  console.log('TEST ERROR: ' + e.message);
  process.exit(2);
});
