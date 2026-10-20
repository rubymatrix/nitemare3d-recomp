/*
 * Browser shell for the WebAssembly build of Nitemare-3D.
 *
 * - Game data comes from the site (data/manifest.json, written by
 *   tools/web_site.py: downloaded into /bundled, tunes streamed in the
 *   background), from a folder bundled into the build (/bundled, private
 *   builds), or from files the player picks once; those are kept in IndexedDB
 *   (/gamedata). Saves and NITE3W.INI live in /user, also in IndexedDB.
 * - The game starts from a click, so audio and pointer lock are allowed.
 * - Sound plays through an AudioWorklet fed by the game (src/host_sdl.c).
 * - URL query parameters become command-line options:
 *     ?classic  ?fm  ?res=1280x720  ?filter=trilinear  ?hidpi  ?game=-b
 *   (?args=... passes a raw option string).
 */
'use strict';

const N3D_REQUIRED = ['map.1', 'img.1', 'snd.dat', 'uif.dat'];
const N3D_KNOWN = new Set(['map.1', 'map.2', 'map.3', 'img.1', 'img.2', 'img.3', 'snd.dat', 'uif.dat',
  'game.pal', 'nite3d.bsf', 'demo.1', 'ending.fli', 'nite3w.ini']);

const $ = (id) => document.getElementById(id);
let Mod = null;
let running = false;

function show(id) {
  for (const s of ['need-data', 'ready', 'ended']) $(s).hidden = s !== id;
}
function status(t) { $('status').textContent = t; }

let syncing = null;
function syncfs(populate) {
  if (syncing) return syncing.then(() => syncfs(populate)); // one at a time
  syncing = new Promise((resolve) => Mod.FS.syncfs(populate, (err) => {
    if (err) console.warn('IndexedDB sync failed', err);
    syncing = null;
    resolve();
  }));
  return syncing;
}

function listDir(path) {
  try { return Mod.FS.readdir(path).filter((n) => n !== '.' && n !== '..'); } catch (e) { return []; }
}
function hasAll(path) {
  const names = new Set(listDir(path).map((n) => n.toLowerCase()));
  return N3D_REQUIRED.filter((n) => !names.has(n));
}

/* ---- picking the game files ---- */
function destFor(name, parentDir) {
  const lower = name.toLowerCase();
  if (/^tune\d\d\.(ogg|wav)$/.test(lower)) {
    // from a folder pick, only take the tunes of a folder named "music"
    if (parentDir && parentDir.toLowerCase() !== 'music') return null;
    return '/gamedata/music/' + lower;
  }
  if (N3D_KNOWN.has(lower) || lower.endsWith('.sf2') || lower.endsWith('.dls')) return '/gamedata/' + lower;
  return null;
}

async function storeFiles(entries) { /* [{file, parent}] */
  let stored = 0;
  for (const { file, parent } of entries) {
    const dest = destFor(file.name, parent);
    if (!dest) continue;
    status('Storing ' + file.name + '…');
    const data = new Uint8Array(await file.arrayBuffer());
    Mod.FS.writeFile(dest, data);
    stored++;
  }
  await syncfs(false);
  return stored;
}

function parentOf(relPath) {
  const parts = (relPath || '').split('/');
  return parts.length > 1 ? parts[parts.length - 2] : '';
}

async function onPicked(fileList) {
  const entries = Array.from(fileList, (f) => ({ file: f, parent: parentOf(f.webkitRelativePath) }));
  await storeFiles(entries);
  checkData();
}

/* drag and drop of files or whole folders */
function readEntry(entry, parent, out) {
  return new Promise((resolve) => {
    if (entry.isFile) {
      entry.file((f) => { out.push({ file: f, parent }); resolve(); }, () => resolve());
    } else if (entry.isDirectory) {
      const reader = entry.createReader();
      const all = [];
      const next = () => reader.readEntries(async (batch) => {
        if (!batch.length) {
          for (const e of all) await readEntry(e, entry.name, out);
          resolve();
        } else { all.push(...batch); next(); }
      }, () => resolve());
      next();
    } else resolve();
  });
}

function setupDrop() {
  const ov = $('overlay');
  window.addEventListener('dragover', (e) => { e.preventDefault(); ov.classList.add('dragging'); });
  window.addEventListener('dragleave', () => ov.classList.remove('dragging'));
  window.addEventListener('drop', async (e) => {
    e.preventDefault();
    ov.classList.remove('dragging');
    if (running) return;
    const out = [];
    for (const item of e.dataTransfer.items) {
      const entry = item.webkitGetAsEntry && item.webkitGetAsEntry();
      if (entry) await readEntry(entry, '', out);
    }
    await storeFiles(out);
    checkData();
  });
  $('pick-files').addEventListener('change', (e) => onPicked(e.target.files));
  $('pick-dir').addEventListener('change', (e) => onPicked(e.target.files));
}

/* ---- starting the game ---- */
let dataDir = null;

function checkData() {
  if (!hasAll('/bundled').length) dataDir = '/bundled';
  else if (!hasAll('/gamedata').length) dataDir = '/gamedata';
  else dataDir = null;
  if (dataDir) {
    status('Ready.');
    $('forget').parentElement.hidden = dataDir !== '/gamedata';
    show('ready');
  } else {
    const missing = hasAll('/gamedata');
    const any = listDir('/gamedata').length > 0;
    status('Game files needed.');
    $('missing').hidden = !any;
    $('missing').textContent = 'Still missing: ' + missing.map((n) => n.toUpperCase()).join(', ');
    show('need-data');
  }
}

/* ---- game data hosted next to the page (tools/web_site.py) ---- */
let siteMusic = false;

function mb(n) { return (n / 1e6).toFixed(1) + ' MB'; }

async function fetchInto(path, url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(url + ': HTTP ' + r.status);
  Mod.FS.writeFile(path, new Uint8Array(await r.arrayBuffer()));
}

async function loadSiteData() {
  let manifest;
  try {
    const r = await fetch('data/manifest.json', { cache: 'no-cache' });
    if (!r.ok) return false;
    manifest = await r.json();
  } catch (e) { return false; }
  const v = '?v=' + encodeURIComponent(manifest.version || '');
  try { Mod.FS.mkdir('/bundled'); } catch (e) { /* exists (bundled build) */ }
  try { Mod.FS.mkdir('/bundled/music'); } catch (e) { /* exists */ }
  const total = manifest.files.reduce((a, f) => a + f.size, 0);
  let done = 0;
  for (const f of manifest.files) {
    status('Downloading game data… ' + mb(done) + ' / ' + mb(total));
    await fetchInto('/bundled/' + f.name, 'data/' + f.name + v);
    done += f.size;
  }
  // tunes: in the background, in order; a tune not here yet plays on the synth
  if (manifest.music && manifest.music.length) {
    siteMusic = true;
    (async () => {
      for (const f of manifest.music) {
        try { await fetchInto('/bundled/music/' + f.name, 'data/music/' + f.name + v); } catch (e) { console.warn(e); }
      }
    })();
  }
  return true;
}

function buildArgs() {
  const args = ['--data', dataDir, '--user', '/user'];
  if (dataDir === '/bundled' && siteMusic) args.push('--music', '/bundled/music');
  const q = new URLSearchParams(location.search);
  let game = '';
  for (const [k, v] of q) {
    if (k === 'args') args.push(...v.split(/\s+/).filter(Boolean));
    else if (k === 'game') game = v;
    else { args.push('--' + k); if (v) args.push(v); }
  }
  // a .dls bank is only used when named (the game finds .sf2 files by itself)
  if (!q.has('soundfont') && !q.has('fm')) {
    const names = listDir(dataDir);
    const dls = names.find((n) => n.toLowerCase().endsWith('.dls'));
    if (dls && !names.some((n) => n.toLowerCase().endsWith('.sf2'))) args.push('--soundfont', dataDir + '/' + dls);
  }
  if (game) args.push('--', ...game.split(/\s+/).filter(Boolean));
  return args;
}

/* ---- mouse capture ----
 * Mouse look needs the pointer lock. The page manages it instead of SDL:
 * - locked straight from the Play click, with raw input (no OS pointer
 *   acceleration) where the browser supports it, for consistent aiming;
 * - a click on the game while unlocked only re-captures, it doesn't fire;
 * - while unlocked, cursor motion doesn't reach the game (no drifting view);
 * - browsers release the lock on Esc and often swallow that key press: the
 *   game then gets its Esc anyway, so Esc opens the menu as on the desktop. */
let mouselook = true;
let lastEsc = 0;

function pointerLocked() { return document.pointerLockElement === $('canvas'); }

function lockPointer() {
  const c = $('canvas');
  if (!mouselook || pointerLocked() || !c.requestPointerLock) return;
  let p;
  try { p = c.requestPointerLock({ unadjustedMovement: true }); } catch (e) { p = null; }
  if (p && p.catch) p.catch((e) => {
    if (e && e.name === 'InUseAttributeError') return; // a request is already pending
    const q = c.requestPointerLock(); // no raw input support: plain lock
    if (q && q.catch) q.catch(() => {});
  });
}

function gamepadConnected() {
  return Array.from(navigator.getGamepads ? navigator.getGamepads() : []).some((g) => g && g.connected);
}

function updateMouseHint() {
  $('mouse-hint').hidden = !(running && mouselook && !pointerLocked() && !document.hidden && !gamepadConnected());
}

function setupMouse() {
  const canvas = $('canvas');
  window.addEventListener('keydown', (e) => { if (e.key === 'Escape') lastEsc = performance.now(); }, true);
  window.addEventListener('mousedown', (e) => {
    if (!running || !mouselook || pointerLocked()) return;
    if (e.target !== canvas && e.target !== $('mouse-hint')) return;
    e.preventDefault();
    e.stopImmediatePropagation();
    lockPointer();
  }, true);
  window.addEventListener('mousemove', (e) => {
    if (running && mouselook && !pointerLocked()) e.stopImmediatePropagation();
  }, true);
  document.addEventListener('pointerlockchange', () => {
    const locked = pointerLocked();
    if (!locked && running && document.hasFocus() && !document.hidden) {
      // Esc released the lock but the page never saw the key press
      setTimeout(() => { if (performance.now() - lastEsc > 250) Mod._n3d_web_esc_swallowed(); }, 50);
    }
    updateMouseHint();
  });
  document.addEventListener('visibilitychange', updateMouseHint);
  window.addEventListener('gamepadconnected', updateMouseHint);
  window.addEventListener('gamepaddisconnected', updateMouseHint);
}

function play() {
  if (running) return;
  running = true;
  $('overlay').hidden = true;
  const canvas = $('canvas');
  canvas.focus();
  mouselook = !new URLSearchParams(location.search).has('no-mouselook');
  lockPointer(); // still inside the click: the browser allows the lock
  const args = buildArgs();
  console.log('nitemare3d', args.join(' '));
  try {
    Mod.callMain(args);
  } catch (e) {
    if (e !== 'unwind' && !(e && e.name === 'ExitStatus')) throw e;
  }
  // persist saves and settings
  setInterval(() => { if (!syncing) syncfs(false); }, 3000);
  document.addEventListener('visibilitychange', () => { if (document.hidden) syncfs(false); });
  window.addEventListener('pagehide', () => syncfs(false));
  // Ctrl+W / Ctrl+R are browser shortcuts the game's Ctrl (fire) can hit
  window.addEventListener('beforeunload', (e) => { if (running) e.preventDefault(); });
}

function onGameQuit() {
  running = false;
  if (document.pointerLockElement) document.exitPointerLock();
  updateMouseHint();
  syncfs(false);
  if (document.fullscreenElement) document.exitFullscreen();
  $('overlay').hidden = false;
  status('');
  show('ended');
}

/* F11: page full screen; while full screen, take the whole keyboard (Esc and
 * Ctrl+W go to the game; hold Esc to leave full screen) where supported. */
function setupFullscreen() {
  window.addEventListener('keydown', (e) => {
    if (e.key !== 'F11') return;
    e.preventDefault();
    e.stopImmediatePropagation();
    if (document.fullscreenElement) document.exitFullscreen();
    else document.documentElement.requestFullscreen().catch(() => {});
  }, true);
  document.addEventListener('fullscreenchange', () => {
    const kb = navigator.keyboard;
    if (!kb || !kb.lock) return;
    if (document.fullscreenElement) kb.lock().catch(() => {});
    else kb.unlock();
  });
}

async function n3dBoot() {
  setupDrop();
  setupFullscreen();
  setupMouse();
  $('play').addEventListener('click', play);
  $('forget').addEventListener('click', async (e) => {
    e.preventDefault();
    for (const n of listDir('/gamedata/music')) Mod.FS.unlink('/gamedata/music/' + n);
    for (const n of listDir('/gamedata')) if (n !== 'music') Mod.FS.unlink('/gamedata/' + n);
    await syncfs(false);
    checkData();
  });
  status('Loading…');
  Mod = await createNitemare3D({
    canvas: $('canvas'),
    print: (t) => console.log(t),
    printErr: (t) => console.warn(t),
    onGameQuit,
  });
  const FS = Mod.FS;
  for (const d of ['/user', '/gamedata']) {
    FS.mkdir(d);
    FS.mount(Mod.IDBFS || FS.filesystems.IDBFS, {}, d);
  }
  await syncfs(true);
  try { FS.mkdir('/gamedata/music'); } catch (e) { /* exists */ }
  try {
    await loadSiteData();
  } catch (e) {
    console.warn('hosted game data failed to load', e);
    status('Could not download the game data (' + e.message + ').');
  }
  checkData();
}
