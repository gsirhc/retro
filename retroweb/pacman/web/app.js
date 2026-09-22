// Pac-Man arcade front end. Guest CPU is always 3.072 MHz vs wall clock.

const CPU_HZ = 3_072_000;
const IDB_NAME = "retroweb-pacman";
const IDB_STORE = "roms";
const TEST = new URLSearchParams(location.search).has("test");
const MSPAC = new URLSearchParams(location.search).get("game") === "mspacman";
const KEEP_HISCORE_KEY = "retroweb.pacman.keepHiscore";
const DIP_KEY = MSPAC ? "retroweb.mspacman.dips" : "retroweb.pacman.dips";
const ROM_KEY = MSPAC ? "set-mspacman" : "set";
const DSW1_FACTORY = 0xC9;
const HISCORE_ADDR = 0x4E88;
const HISCORE_LEN = 3;

const MAME_PACMAN = {
  "pacman.6e": 0xc1e6ab10,
  "pacman.6f": 0x1a6fb2d4,
  "pacman.6h": 0xbcdd1beb,
  "pacman.6j": 0x817d94e3,
  "pacman.5e": 0x0c944964,
  "pacman.5f": 0x958fedf9,
  "82s123.7f": 0x2fc650bd,
  "82s126.4a": 0x3eb3a8e4,
  "82s126.1m": 0xa9cc86bf,
};

const MAME_MSPACMAN = {
  "5e": 0x5c281d01,
  "5f": 0x615af909,
  "u5": 0xf45fbbcd,
  "u6": 0xa90e7000,
  "u7": 0xc82cd714,
};

function crc32(bytes) {
  let c = ~0;
  for (let i = 0; i < bytes.length; i++) {
    c ^= bytes[i];
    for (let j = 0; j < 8; j++) c = (c >>> 1) ^ (0xEDB88320 & -(c & 1));
  }
  return (~c) >>> 0;
}

async function unzip(buf) {
  const files = {};
  const v = new DataView(buf);
  let i = 0;
  const u8 = new Uint8Array(buf);
  while (i + 30 <= u8.length) {
    if (v.getUint32(i, true) !== 0x04034b50) break;
    const method = v.getUint16(i + 8, true);
    const compSz = v.getUint32(i + 18, true);
    const nameLen = v.getUint16(i + 26, true);
    const extraLen = v.getUint16(i + 28, true);
    const name = new TextDecoder().decode(u8.subarray(i + 30, i + 30 + nameLen));
    const start = i + 30 + nameLen + extraLen;
    const slice = u8.subarray(start, start + compSz);
    let out;
    if (method === 0) out = slice.slice();
    else if (method === 8) {
      const ds = new DecompressionStream("deflate-raw");
      const stream = new Blob([slice]).stream().pipeThrough(ds);
      out = new Uint8Array(await new Response(stream).arrayBuffer());
    } else throw new Error("unsupported zip compression");
    files[name.split("/").pop().toLowerCase()] = out;
    i = start + compSz;
  }
  return files;
}

function idb() {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(IDB_NAME, 1);
    req.onupgradeneeded = () => req.result.createObjectStore(IDB_STORE);
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

async function idbGet(key) {
  const db = await idb();
  return new Promise((resolve, reject) => {
    const r = db.transaction(IDB_STORE, "readonly").objectStore(IDB_STORE).get(key);
    r.onsuccess = () => resolve(r.result || null);
    r.onerror = () => reject(r.error);
  });
}

async function idbSet(key, val) {
  const db = await idb();
  return new Promise((resolve, reject) => {
    const r = db.transaction(IDB_STORE, "readwrite").objectStore(IDB_STORE).put(val, key);
    r.onsuccess = () => resolve();
    r.onerror = () => reject(r.error);
  });
}

async function idbDel(key) {
  const db = await idb();
  return new Promise((resolve, reject) => {
    const r = db.transaction(IDB_STORE, "readwrite").objectStore(IDB_STORE).delete(key);
    r.onsuccess = () => resolve();
    r.onerror = () => reject(r.error);
  });
}

function concat4(parts) {
  const out = new Uint8Array(0x4000);
  for (let i = 0; i < 4; i++) out.set(parts[i], i * 4096);
  return out;
}

function hexCrc(bytes) { return crc32(bytes).toString(16).padStart(8, "0"); }

// Board locations on the original Midway PCB, as they appear in MAME names
// (pacman.6e, puckman.6e, 82s123.7f, …). 82s126.3m is a timing PROM the
// hardware never reads — ignore it.
const CHIP_RE = /(?:^|[._-])(6e|6f|6h|6j|5e|5f|7f|4a|1m|u5|u6|u7)(?:[^a-z0-9]|$)/i;
const IGNORE_RE = /(?:^|[._-])3m(?:[^a-z0-9]|$)/i;
const SIZE = {
  program: 0x4000, bank: 0x1000, tiles: 0x1000, sprites: 0x1000,
  color: 0x20, lookup: 0x100, wave: 0x100, u5: 0x0800, u6: 0x1000, u7: 0x1000,
};

function clip(bytes, n) {
  if (!bytes || bytes.length < n) return null;
  return bytes.length === n ? bytes : bytes.subarray(0, n);
}

function roleOf(name) {
  const n = String(name).toLowerCase().split("/").pop();
  if (n === "program.bin" || n === "prg.bin") return "program";
  if (IGNORE_RE.test(n)) return "ignore";
  const m = n.match(CHIP_RE);
  return m ? m[1].toLowerCase() : null;
}

function identifySet(files, hwtestCrcs) {
  const found = {};
  for (const [name, data] of Object.entries(files)) {
    const role = roleOf(name);
    if (!role || role === "ignore" || found[role]) continue;
    found[role] = data;
  }
  let program = clip(found.program, SIZE.program);
  if (!program) {
    const banks = ["6e", "6f", "6h", "6j"].map((c) => clip(found[c], SIZE.bank));
    if (banks.every(Boolean)) program = concat4(banks);
  }
  const tiles = clip(found["5e"], SIZE.tiles);
  const sprites = clip(found["5f"], SIZE.sprites);
  const color = clip(found["7f"], SIZE.color);
  const lookup = clip(found["4a"], SIZE.lookup);
  const wave = clip(found["1m"], SIZE.wave);
  if (!program || !tiles || !sprites || !color || !lookup || !wave) return null;

  const u5 = clip(found.u5, SIZE.u5);
  const u6 = clip(found.u6, SIZE.u6);
  const u7 = clip(found.u7, SIZE.u7);
  const aux = !!(u5 && u6 && u7);

  let kind = "user";
  const progCrc = hexCrc(program);
  if (hwtestCrcs &&
      (progCrc === hwtestCrcs["program.bin"] || progCrc === hwtestCrcs["mspacman-program.bin"]) &&
      hexCrc(tiles) === hwtestCrcs["pacman.5e"] &&
      hexCrc(sprites) === hwtestCrcs["pacman.5f"] &&
      hexCrc(color) === hwtestCrcs["82s123.7f"] &&
      hexCrc(lookup) === hwtestCrcs["82s126.4a"] &&
      hexCrc(wave) === hwtestCrcs["82s126.1m"]) {
    kind = "hwtest";
  } else if (
    crc32(color) === MAME_PACMAN["82s123.7f"] &&
    crc32(lookup) === MAME_PACMAN["82s126.4a"] &&
    crc32(wave) === MAME_PACMAN["82s126.1m"] &&
    crc32(program.subarray(0, 0x1000)) === MAME_PACMAN["pacman.6e"] &&
    crc32(program.subarray(0x1000, 0x2000)) === MAME_PACMAN["pacman.6f"] &&
    crc32(program.subarray(0x2000, 0x3000)) === MAME_PACMAN["pacman.6h"] &&
    crc32(program.subarray(0x3000, 0x4000)) === MAME_PACMAN["pacman.6j"]
  ) {
    if (aux &&
        crc32(tiles) === MAME_MSPACMAN["5e"] &&
        crc32(sprites) === MAME_MSPACMAN["5f"] &&
        crc32(u5) === MAME_MSPACMAN.u5 &&
        crc32(u6) === MAME_MSPACMAN.u6 &&
        crc32(u7) === MAME_MSPACMAN.u7) {
      kind = "mame-mspacman";
    } else if (
      crc32(tiles) === MAME_PACMAN["pacman.5e"] &&
      crc32(sprites) === MAME_PACMAN["pacman.5f"]
    ) {
      kind = "mame-pacman";
    }
  }
  return { kind, program, tiles, sprites, color, lookup, wave, u5, u6, u7, aux };
}

function labelFor(kind) {
  if (kind === "hwtest") return "Hardware test ROM";
  if (kind === "mame-pacman") return "Midway pacman ROM set";
  if (kind === "mame-mspacman") return "Midway mspacman ROM set";
  return "Loaded ROM set";
}

function applyChrome() {
  if (!MSPAC) return;
  document.title = "Ms. Pac-Man Arcade";
  const name = document.querySelector(".pb-name");
  if (name) name.textContent = "Ms. Pac-Man Arcade";
  const h1 = document.querySelector("h1");
  if (h1) h1.textContent = "Ms. Pac-Man Arcade";
  const tag = document.querySelector(".tagline");
  if (tag) {
    tag.textContent = "Midway Ms. Pac-Man (1982) — Pac-Man PCB plus the GCC aux board in the Z80 socket.";
  }
  const pb = document.querySelector(".pb-title");
  if (pb) {
    const icon = pb.querySelector("img");
    pb.textContent = "";
    if (icon) pb.appendChild(icon);
    pb.appendChild(document.createTextNode(" MSPACMAN"));
  }
  const dipLegal = document.getElementById("dipLegal");
  if (dipLegal) {
    dipLegal.textContent = "Same operator bank as inside the cabinet (Midway service manual). Settings survive a refresh the way the physical switches do. Difficulty is a solder pad on some boards; there is no ghost-names pad. Cabinet type is an edge-connector jumper. The game samples these on the next credit.";
  }
  const legal = document.getElementById("romLegal");
  if (legal) {
    legal.innerHTML = "This page ships a hardware self-test ROM, not Namco/Midway Ms. Pac-Man — those ROMs are still under copyright and are <strong>not</strong> included or downloaded. Load a complete original conversion-kit set (MAME <code>mspacman</code> zip, or the loose chips: Pac-Man program 6e/6f/6h/6j, aux board <code>u5</code>/<code>u6</code>/<code>u7</code>, and Ms. Pac-Man 5e/5f graphics). Each chip is identified by its usual name and checked by size, not a particular CRC; extra files are ignored. The set stays in this browser.";
  }
  const err = document.getElementById("romErrorCopy");
  if (err) {
    err.innerHTML = "Need a complete Ms. Pac-Man conversion-kit set — a MAME <code>mspacman</code> zip, or the loose chips (Pac-Man 16K program or four 4K banks, aux board U5 2K + U6/U7 4K, 4K tiles, 4K sprites, color and wave PROMs). Usual names like <code>pacman.6e</code> / <code>u5</code> / <code>5e</code> / <code>82s123.7f</code>. Each chip is checked by size, not a particular CRC; extra files are ignored.";
  }
  const ghosts = document.getElementById("dipGhostsRow");
  if (ghosts) ghosts.hidden = true;
}

applyChrome();

initThemePicker();
initFullscreen({
  bezelEl: document.getElementById("bezel"),
  screenEl: document.getElementById("screen"),
  fullscreenBtn: document.getElementById("fullscreenBtn"),
  isRunning: () => true,
});
// initFocusHint only updates the banner on later focusin/focusout events --
// call it once now too, since nothing here calls screen.focus() on load the
// way altair8800/assembler6502's terminal-focusing boot flow does, so the
// hint needs an explicit nudge to reflect "nothing is focused yet" from the
// very first frame (same reasoning as ibmpc-at's own post-power-on call).
const updateFocusHint = initFocusHint(document.getElementById("screen"), () => true);
updateFocusHint();

PacmanArcade().then(async (Module) => {
  const machine = new Module.Machine();
  if (MSPAC) machine.loadMsHwtest();
  const screen = document.getElementById("screen");
  const ctx = screen.getContext("2d");
  const img = ctx.createImageData(224, 288);
  const status = document.getElementById("romStatus");
  const mute = document.getElementById("mute");
  const keepHiscore = document.getElementById("keepHiscore");

  let usingUserRom = false;
  let programCrc = 0;
  let hiscoreRestored = false;
  let lastSavedHiscore = "";
  let in0 = 0xFF, in1 = 0xFF;
  let rackTest = false;
  let cocktail = false;
  const keys = {};
  let coinUntil = 0;
  const coinDoor = document.getElementById("coinDoor");

  function applyKeys() {
    let n0 = 0xFF, n1 = 0xFF;
    const down = (k) => keys[k];
    if (down("ArrowUp") || down("KeyW")) n0 &= ~0x01;
    if (down("ArrowLeft") || down("KeyA")) n0 &= ~0x02;
    if (down("ArrowRight") || down("KeyD")) n0 &= ~0x04;
    if (down("ArrowDown") || down("KeyS")) n0 &= ~0x08;
    if (rackTest) n0 &= ~0x10;
    if (down("Digit5") || down("Numpad5") || Date.now() < coinUntil) n0 &= ~0x20;
    if (down("Digit1") || down("Numpad1")) n1 &= ~0x20;
    if (down("Digit2") || down("Numpad2")) n1 &= ~0x40;
    if (cocktail) n1 &= ~0x80;
    in0 = n0; in1 = n1;
    machine.setIn0(in0);
    machine.setIn1(in1);
  }

  function insertCoin() {
    coinUntil = Date.now() + 120;
    applyKeys();
    window.setTimeout(applyKeys, 130);
    coinDoor.classList.add("coined");
    window.setTimeout(() => coinDoor.classList.remove("coined"), 180);
    screen.focus();
  }
  coinDoor.querySelectorAll("[data-coin]").forEach((el) => {
    el.addEventListener("click", insertCoin);
  });
  document.querySelectorAll("[data-start]").forEach((el) => {
    const code = el.getAttribute("data-start") === "2" ? "Digit2" : "Digit1";
    const down = (e) => {
      if (e.button != null && e.button !== 0) return;
      e.preventDefault();
      try { el.setPointerCapture(e.pointerId); } catch {}
      el.classList.add("pressed");
      keys[code] = true;
      applyKeys();
      screen.focus();
    };
    const up = () => {
      el.classList.remove("pressed");
      keys[code] = false;
      applyKeys();
    };
    el.addEventListener("pointerdown", down);
    el.addEventListener("pointerup", up);
    el.addEventListener("pointercancel", up);
  });

  window.addEventListener("keydown", (e) => {
    keys[e.code] = true;
    applyKeys();
    if (["ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight"].includes(e.code)) e.preventDefault();
  });
  window.addEventListener("keyup", (e) => {
    keys[e.code] = false;
    applyKeys();
  });

  function blit() {
    const rgba = machine.frameBuffer();
    img.data.set(rgba);
    ctx.putImageData(img, 0, 0);
  }

  let audioCtx = null, audioNode = null;
  async function ensureAudio() {
    if (mute.checked) return;
    if (audioCtx) {
      if (audioCtx.state === "suspended") await audioCtx.resume();
      return;
    }
    audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    // The WSG's real 96 kHz clock resamples to whatever the actual output
    // device rate is -- don't assume 48 kHz, some devices differ.
    machine.setAudioHz(audioCtx.sampleRate);
    if (!audioCtx.audioWorklet) return;
    const src = `registerProcessor("wsg", class extends AudioWorkletProcessor {
      constructor() { super(); this.q = []; this.i = 0; this.port.onmessage = e => { this.q.push(e.data); }; }
      process(_, outputs) {
        const o = outputs[0][0];
        for (let i = 0; i < o.length; i++) {
          if (this.q.length && this.i >= this.q[0].length) { this.q.shift(); this.i = 0; }
          o[i] = this.q.length ? this.q[0][this.i++] : 0;
        }
        return true;
      }
    });`;
    const url = URL.createObjectURL(new Blob([src], { type: "text/javascript" }));
    await audioCtx.audioWorklet.addModule(url);
    audioNode = new AudioWorkletNode(audioCtx, "wsg");
    audioNode.connect(audioCtx.destination);
  }

  document.addEventListener("click", () => { ensureAudio().catch(() => {}); }, { once: true });

  function pumpAudio() {
    if (!audioNode || mute.checked) {
      machine.drainAudio();
      return;
    }
    const samples = machine.drainAudio();
    if (samples.length) audioNode.port.postMessage(samples, [samples.buffer]);
  }

  function setStatus(t) { status.textContent = t; }

  const romErrorHint = document.getElementById("romErrorHint");
  document.getElementById("romErrorOk")?.addEventListener("click", () => romErrorHint?.close());
  function showRomError() {
    if (romErrorHint && !romErrorHint.open) romErrorHint.showModal();
  }
  function hideRomError() {
    if (romErrorHint && romErrorHint.open) romErrorHint.close();
  }

  function keepingHiscore() { return !!(keepHiscore && keepHiscore.checked); }

  function readHiscore() {
    const b = [];
    for (let i = 0; i < HISCORE_LEN; i++) b.push(machine.ramByte(HISCORE_ADDR + i) & 0xff);
    return b;
  }

  function writeHiscore(bytes) {
    if (!bytes || bytes.length < HISCORE_LEN) return;
    for (let i = 0; i < HISCORE_LEN; i++) machine.setRamByte(HISCORE_ADDR + i, bytes[i] & 0xff);
  }

  function hiscoreIsZero(b) { return !b[0] && !b[1] && !b[2]; }

  async function loadSavedHiscores() {
    const all = await idbGet("hiscores");
    return (all && typeof all === "object") ? all : {};
  }

  async function saveHiscoreNow(bytes) {
    const all = await loadSavedHiscores();
    all[programCrc >>> 0] = [bytes[0] & 0xff, bytes[1] & 0xff, bytes[2] & 0xff];
    await idbSet("hiscores", all);
    lastSavedHiscore = bytes.join(",");
  }

  let restoreInFlight = false;
  async function maybeRestoreHiscore() {
    if (!keepingHiscore() || !usingUserRom || restoreInFlight) return;
    // POST zeros work RAM, including TOP. Wait for attract ($4E00 == 1)
    // and only write back if the ROM still has a zero high score.
    if (machine.ramByte(0x4E00) !== 1) return;
    if (!hiscoreIsZero(readHiscore())) {
      hiscoreRestored = true;
      return;
    }
    restoreInFlight = true;
    try {
      const all = await loadSavedHiscores();
      const saved = all[programCrc >>> 0];
      if (saved && saved.length >= HISCORE_LEN) writeHiscore(saved);
      hiscoreRestored = true;
      lastSavedHiscore = readHiscore().join(",");
    } finally {
      restoreInFlight = false;
    }
  }

  let saveInFlight = false;
  async function maybeSaveHiscore() {
    if (!keepingHiscore() || !usingUserRom || saveInFlight) return;
    const cur = readHiscore();
    if (hiscoreIsZero(cur) && !hiscoreRestored) return;
    const key = cur.join(",");
    if (key === lastSavedHiscore) return;
    saveInFlight = true;
    try { await saveHiscoreNow(cur); }
    finally { saveInFlight = false; }
  }

  try {
    keepHiscore.checked = localStorage.getItem(KEEP_HISCORE_KEY) === "1";
  } catch {}
  keepHiscore.addEventListener("change", () => {
    try { localStorage.setItem(KEEP_HISCORE_KEY, keepHiscore.checked ? "1" : "0"); } catch {}
    if (!keepHiscore.checked) hiscoreRestored = false;
  });

  const dipCoinage = document.getElementById("dipCoinage");
  const dipLives = document.getElementById("dipLives");
  const dipBonus = document.getElementById("dipBonus");
  const dipDifficulty = document.getElementById("dipDifficulty");
  const dipGhosts = document.getElementById("dipGhosts");
  const dipCabinet = document.getElementById("dipCabinet");
  const dipRack = document.getElementById("dipRack");

  function encodeDsw1() {
    let v = (Number(dipCoinage.value) | Number(dipLives.value) | Number(dipBonus.value) |
            Number(dipDifficulty.value) | Number(dipGhosts.value)) & 0xff;
    // Ms. Pac-Man has no ghost-names pad; DSW1 bit 7 is unused and reads 1.
    if (MSPAC) v |= 0x80;
    return v;
  }

  function applyDips() {
    machine.setDsw1(encodeDsw1());
    rackTest = dipRack.value === "1";
    cocktail = dipCabinet.value === "1";
    applyKeys();
  }

  function saveDips() {
    try {
      localStorage.setItem(DIP_KEY, JSON.stringify({
        dsw1: encodeDsw1(),
        rackTest: dipRack.value === "1",
        cocktail: dipCabinet.value === "1",
      }));
    } catch {}
    applyDips();
  }

  function loadDips() {
    let dsw1 = DSW1_FACTORY;
    try {
      const raw = localStorage.getItem(DIP_KEY);
      if (raw) {
        const s = JSON.parse(raw);
        if (typeof s.dsw1 === "number") dsw1 = s.dsw1 & 0xff;
        dipRack.value = s.rackTest ? "1" : "0";
        dipCabinet.value = s.cocktail ? "1" : "0";
      }
    } catch {}
    dipCoinage.value = String(dsw1 & 0x03);
    dipLives.value = String(dsw1 & 0x0c);
    dipBonus.value = String(dsw1 & 0x30);
    dipDifficulty.value = String(dsw1 & 0x40);
    dipGhosts.value = String(dsw1 & 0x80);
    applyDips();
  }

  [dipCoinage, dipLives, dipBonus, dipDifficulty, dipGhosts, dipCabinet, dipRack]
    .forEach((el) => el.addEventListener("change", saveDips));
  loadDips();

  function applySet(set, label) {
    if (!set || !set.program || !set.tiles || !set.sprites || !set.color || !set.lookup || !set.wave)
      throw new Error("incomplete ROM set");
    if (MSPAC) {
      if (!set.aux) throw new Error("Ms. Pac-Man aux board needs U5, U6, and U7");
      machine.loadMsPacmanSet(set.program, set.tiles, set.sprites, set.color, set.lookup, set.wave,
        set.u5, set.u6, set.u7);
    } else {
      machine.loadRomSet(set.program, set.tiles, set.sprites, set.color, set.lookup, set.wave);
    }
    usingUserRom = set.kind !== "hwtest";
    programCrc = crc32(set.program);
    if (set.aux) programCrc = crc32(set.u5) ^ crc32(set.u6) ^ crc32(set.u7) ^ programCrc;
    hiscoreRestored = false;
    lastSavedHiscore = "";
    setStatus(label);
  }

  let hwtestCrcs = null;
  try { hwtestCrcs = await (await fetch("roms/crc.json")).json(); } catch {}

  function loadBuiltInRom() {
    if (MSPAC) machine.loadMsHwtest();
    else machine.loadHwtest();
    usingUserRom = false;
    hiscoreRestored = false;
    setStatus("Running test ROM");
  }

  const stored = await idbGet(ROM_KEY);
  if (stored) {
    try { applySet(stored, "Loaded stored ROM set"); }
    catch (e) {
      loadBuiltInRom();
      setStatus("Stored ROM unreadable — using test ROM");
      showRomError();
    }
  }

  function bufOf(bytes) {
    if (bytes instanceof ArrayBuffer) return bytes;
    if (ArrayBuffer.isView(bytes)) {
      return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
    }
    return bytes;
  }

  async function ingestRomFiles(items) {
    const files = {};
    for (const f of items) {
      const name = f.name.toLowerCase();
      // File/Blob.bytes is a method on current Chromium; Drive items carry
      // a Uint8Array on .bytes. Only treat the latter as payload.
      const bytes = f.bytes instanceof Uint8Array
        ? f.bytes
        : new Uint8Array(await f.arrayBuffer());
      if (name.endsWith(".zip")) Object.assign(files, await unzip(bufOf(bytes)));
      else files[name] = bytes;
    }
    const set = identifySet(files, hwtestCrcs);
    if (!set) throw new Error("incomplete or wrong-sized ROM set");
    applySet(set, labelFor(set.kind));
    await idbSet(ROM_KEY, set);
    hideRomError();
  }

  document.getElementById("loadRomBtn").addEventListener("click", () => {
    document.getElementById("romFile").click();
  });

  document.getElementById("romFile").addEventListener("change", async (ev) => {
    const list = [...ev.target.files];
    ev.target.value = "";
    try {
      await ingestRomFiles(list);
    } catch (e) {
      setStatus("ROM rejected: " + e.message);
      showRomError();
    }
  });

  document.getElementById("loadRomDriveBtn").addEventListener("click", async () => {
    const gd = window.RetroGdrive;
    if (!gd) return;
    try {
      const items = await gd.pickAndDownloadRoms({ setStatus });
      if (!items || !items.length) return;
      await ingestRomFiles(items);
    } catch (e) {
      setStatus("ROM rejected: " + e.message);
      showRomError();
    }
  });

  document.getElementById("removeRomBtn").addEventListener("click", async () => {
    await idbDel(ROM_KEY);
    loadBuiltInRom();
  });

  let lastT = null;
  function tick(t) {
    if (lastT === null) lastT = t;
    let dt = (t - lastT) / 1000;
    lastT = t;
    if (dt > 0.08) dt = 0.08;
    const cycles = Math.floor(CPU_HZ * dt);
    if (cycles > 0) machine.runCycles(cycles);
    maybeRestoreHiscore().catch(() => {});
    maybeSaveHiscore().catch(() => {});
    blit();
    pumpAudio();
    requestAnimationFrame(tick);
  }
  requestAnimationFrame(tick);

  if (TEST) {
    window.__test = {
      machine,
      get status() { return status.textContent; },
      get usingUserRom() { return usingUserRom; },
      keepHiscore,
      mute,
      get muted() { return !!mute.checked; },
      encodeDsw1,
      readHiscore,
      writeHiscore,
      saveHiscoreNow,
      restoreHiscoreNow: async () => {
        // Same gates as maybeRestoreHiscore, but skip the attract wait —
        // the generated self-test ROM (even CRC-patched as a "user" set)
        // never sets $4E00 == 1.
        if (!keepingHiscore() || !usingUserRom) return false;
        const all = await loadSavedHiscores();
        const saved = all[programCrc >>> 0];
        if (saved && saved.length >= HISCORE_LEN) writeHiscore(saved);
        hiscoreRestored = true;
        lastSavedHiscore = readHiscore().join(",");
        return true;
      },
      crc32,
      identifySet,
      hwtestCrcs,
      applySet,
      MSPAC,
      ROM_KEY,
    };
  }
});
