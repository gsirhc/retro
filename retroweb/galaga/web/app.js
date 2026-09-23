// Midway Galaga front end. Each Z80 stays at 3.072 MHz vs wall clock.

const CPU_HZ = 3_072_000;
const IDB_NAME = "retroweb-galaga";
const IDB_STORE = "roms";
const TEST = new URLSearchParams(location.search).has("test");
const DIP_KEY = "retroweb.galaga.dips";
const ROM_KEY = "set";
// MAME hiscore.dat galaga group (galagamw): work RAM then the score strip.
const HISCORE = [
  { addr: 0x8A20, len: 0x2D },
  { addr: 0x83ED, len: 6 },
];
const HISCORE_LEN = 0x2D + 6;
// Midway reset table starts as the character bytes for a 20000 score.
const FACTORY_PREFIX = [0x00, 0x00, 0x00, 0x00, 0x02, 0x24];

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
    const tx = db.transaction(IDB_STORE, "readwrite");
    tx.objectStore(IDB_STORE).put(val, key);
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
    tx.onabort = () => reject(tx.error);
  });
}

async function idbDel(key) {
  const db = await idb();
  return new Promise((resolve, reject) => {
    const tx = db.transaction(IDB_STORE, "readwrite");
    tx.objectStore(IDB_STORE).delete(key);
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
    tx.onabort = () => reject(tx.error);
  });
}

function hexCrc(bytes) { return crc32(bytes).toString(16).padStart(8, "0"); }

function exact(bytes, n) {
  if (!bytes || bytes.length !== n) return null;
  return bytes;
}

function concat(parts, total) {
  const out = new Uint8Array(total);
  let o = 0;
  for (const p of parts) {
    out.set(p, o);
    o += p.length;
  }
  return out;
}

// Midway galagamw socket names. CRC only tells the self-test from a user set.
function roleOf(name) {
  const n = String(name).toLowerCase().split("/").pop();
  const roles = {
    "main.bin": "main",
    "sub.bin": "sub",
    "sound.bin": "sound",
    "3200a.bin": "a", "3200a": "a",
    "3300b.bin": "b", "3300b": "b",
    "3400c.bin": "c", "3400c": "c",
    "3500d.bin": "d", "3500d": "d",
    "3600e.bin": "sub", "3600e": "sub",
    "3700g.bin": "sound", "3700g": "sound",
    "2600j.bin": "tiles", "2600j": "tiles",
    "sprites.bin": "sprites",
    "2800l.bin": "spr0", "2800l": "spr0",
    "2700k.bin": "spr1", "2700k": "spr1",
    "prom-5.5n": "palette",
    "prom-4.2n": "charLut",
    "prom-3.1c": "spriteLut",
    "prom-1.1d": "wave",
    "51xx.bin": "mcu51",
    "54xx.bin": "mcu54",
  };
  return roles[n] || null;
}

function identifySet(files, hwtestCrcs) {
  const found = {};
  for (const [name, data] of Object.entries(files)) {
    const role = roleOf(name);
    if (!role || found[role]) continue;
    found[role] = data;
  }
  let main = exact(found.main, 0x4000);
  if (!main) {
    const chips = ["a", "b", "c", "d"].map((k) => exact(found[k], 0x1000));
    if (chips.every(Boolean)) main = concat(chips, 0x4000);
  }
  const sub = exact(found.sub, 0x1000);
  const sound = exact(found.sound, 0x1000);
  const tiles = exact(found.tiles, 0x1000);
  let sprites = exact(found.sprites, 0x2000);
  if (!sprites) {
    const a = exact(found.spr0, 0x1000);
    const b = exact(found.spr1, 0x1000);
    if (a && b) sprites = concat([a, b], 0x2000);
  }
  const palette = exact(found.palette, 0x20);
  const charLut = exact(found.charLut, 0x100);
  const spriteLut = exact(found.spriteLut, 0x100);
  const wave = exact(found.wave, 0x100);
  if (!main || !sub || !sound || !tiles || !sprites || !palette || !charLut || !spriteLut || !wave) {
    return null;
  }
  const mcu51 = exact(found.mcu51, 0x400);
  const mcu54 = exact(found.mcu54, 0x400);
  const set = { kind: "user", main, sub, sound, tiles, sprites, palette, charLut, spriteLut, wave, mcu51, mcu54 };
  if (hwtestCrcs &&
      hexCrc(main) === hwtestCrcs["main.bin"] &&
      hexCrc(sub) === hwtestCrcs["sub.bin"] &&
      hexCrc(sound) === hwtestCrcs["sound.bin"] &&
      hexCrc(tiles) === hwtestCrcs["2600j.bin"] &&
      hexCrc(sprites) === hwtestCrcs["sprites.bin"] &&
      hexCrc(palette) === hwtestCrcs["prom-5.5n"] &&
      hexCrc(charLut) === hwtestCrcs["prom-4.2n"] &&
      hexCrc(spriteLut) === hwtestCrcs["prom-3.1c"] &&
      hexCrc(wave) === hwtestCrcs["prom-1.1d"]) {
    set.kind = "hwtest";
  }
  return set;
}

function hleLine(set) {
  const parts = [];
  if (!set.mcu51) parts.push("51xx.bin missing — 51XX fell back to HLE");
  if (!set.mcu54) parts.push("54xx.bin missing — 54XX fell back to HLE");
  return parts.join(". ");
}

function statusFor(set, stored) {
  if (set.kind === "hwtest") return stored ? "Loaded stored ROM set" : "Running test ROM";
  const hle = hleLine(set);
  if (hle) return hle;
  return stored ? "Loaded stored ROM set" : "Loaded ROM set";
}

initThemePicker();
initFullscreen({
  bezelEl: document.getElementById("bezel"),
  screenEl: document.getElementById("screen"),
  fullscreenBtn: document.getElementById("fullscreenBtn"),
  isRunning: () => true,
});
const updateFocusHint = initFocusHint(document.getElementById("screen"), () => true);
updateFocusHint();

GalagaArcade().then(async (Module) => {
  const machine = new Module.Machine();
  const screen = document.getElementById("screen");
  const ctx = screen.getContext("2d");
  const img = ctx.createImageData(224, 288);
  const status = document.getElementById("romStatus");
  const mute = document.getElementById("mute");
  const resetHiscore = document.getElementById("resetHiscore");
  const hiscoreResetHint = document.getElementById("hiscoreResetHint");

  let usingUserRom = false;
  let programCrc = 0;
  let hiscoreRestored = false;
  let lastSavedHiscore = "";
  const keys = {};
  const coinDoor = document.getElementById("coinDoor");
  const dipCredits = document.getElementById("dipCredits");
  const dipDifficulty = document.getElementById("dipDifficulty");
  const dipDemo = document.getElementById("dipDemo");
  const dipFreeze = document.getElementById("dipFreeze");
  const dipRack = document.getElementById("dipRack");
  const dipCabinet = document.getElementById("dipCabinet");
  const dipCoinage = document.getElementById("dipCoinage");
  const dipBonus = document.getElementById("dipBonus");
  const dipLives = document.getElementById("dipLives");
  const dipEls = [dipCredits, dipDifficulty, dipDemo, dipFreeze, dipRack, dipCabinet, dipCoinage, dipBonus, dipLives];

  function padBits() {
    const pads = navigator.getGamepads ? navigator.getGamepads() : [];
    const p = pads && pads[0];
    if (!p) return { left: false, right: false, fire: false, start: false, coin: false };
    const ax = p.axes && p.axes.length ? p.axes[0] : 0;
    const btn = (i) => !!(p.buttons && p.buttons[i] && p.buttons[i].pressed);
    return {
      left: ax < -0.5 || btn(14),
      right: ax > 0.5 || btn(15),
      fire: btn(0),
      start: btn(9),
      coin: btn(8),
    };
  }

  function applyKeys() {
    // Active-high contacts. The 51XX sees them inverted. Cocktail player 2
    // (IN0 bits 5 and 7, IN1 bit 1) stays unmapped.
    const down = (k) => keys[k];
    const pad = padBits();
    let n0 = 0;
    let n1 = 0;
    if (down("ArrowLeft") || down("KeyA") || pad.left) n0 |= 0x08;
    if (down("ArrowRight") || down("KeyD") || pad.right) n0 |= 0x02;
    if (down("Space") || down("KeyZ") || pad.fire) n1 |= 0x01;
    if (down("Digit5") || down("Numpad5") || pad.coin) n1 |= 0x10;
    if (down("Digit6") || down("Numpad6")) n1 |= 0x20;
    if (down("Digit1") || down("Numpad1") || pad.start) n1 |= 0x04;
    if (down("Digit2") || down("Numpad2")) n1 |= 0x08;
    machine.setIn0(n0);
    machine.setIn1(n1);
    // SWB bit 6 is the unused switch, open, so it reads 1. galagamw's
    // sub CPU resets at $0ECA when $6806 bit 1 is clear.
    const a = (Number(dipCredits.value) & 0x01) |
            (Number(dipDifficulty.value) & 0x06) |
            (Number(dipDemo.value) & 0x08) |
            (Number(dipFreeze.value) & 0x10) |
            (Number(dipRack.value) & 0x20) |
            0x40 |
            (Number(dipCabinet.value) & 0x80);
    const b = (Number(dipCoinage.value) & 0x07) |
            (Number(dipBonus.value) & 0x38) |
            (Number(dipLives.value) & 0xC0);
    machine.setDswA(a);
    machine.setDswB(b);
  }

  function insertCoin(which) {
    const code = which === "2" ? "Digit6" : "Digit5";
    keys[code] = true;
    applyKeys();
    window.setTimeout(() => {
      keys[code] = false;
      applyKeys();
    }, 130);
    coinDoor.classList.add("coined");
    window.setTimeout(() => coinDoor.classList.remove("coined"), 180);
    screen.focus();
    ensureAudio().catch(() => {});
  }
  coinDoor.querySelectorAll("[data-coin]").forEach((el) => {
    el.addEventListener("click", () => insertCoin(el.getAttribute("data-coin")));
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
      ensureAudio().catch(() => {});
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
    if (["ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "Space"].includes(e.code)) e.preventDefault();
    ensureAudio().catch(() => {});
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
    audioCtx = new (window.AudioContext || window.webkitAudioContext)({ latencyHint: "interactive" });
    await audioCtx.resume();
    machine.setAudioHz(audioCtx.sampleRate);
    machine.drainAudio();
    if (!audioCtx.audioWorklet) return;
    const src = `registerProcessor("ay", class extends AudioWorkletProcessor {
      constructor() {
        super();
        this.q = [];
        this.i = 0;
        this.port.onmessage = e => {
          // Drop backlog so a suspended context cannot pile up seconds of delay.
          if (this.q.length > 2) { this.q = []; this.i = 0; }
          this.q.push(e.data);
        };
      }
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
    audioNode = new AudioWorkletNode(audioCtx, "ay");
    audioNode.connect(audioCtx.destination);
  }

  document.addEventListener("click", () => { ensureAudio().catch(() => {}); }, { once: true });

  function pumpAudio() {
    if (!audioNode || mute.checked || !audioCtx || audioCtx.state !== "running") {
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

  function syncHiscoreResetBtn() {
    if (resetHiscore) resetHiscore.disabled = !usingUserRom;
  }

  function readHiscore() {
    const b = [];
    for (const r of HISCORE) {
      for (let i = 0; i < r.len; i++) b.push(machine.ramByte(r.addr + i) & 0xff);
    }
    return b;
  }

  function writeHiscore(bytes) {
    if (!bytes || bytes.length < HISCORE_LEN) return;
    let o = 0;
    for (const r of HISCORE) {
      for (let i = 0; i < r.len; i++) machine.setRamByte(r.addr + i, bytes[o++] & 0xff);
    }
  }

  function hiscoreIsFactory(b) {
    if (b.every((x) => !x)) return true;
    return FACTORY_PREFIX.every((x, i) => b[i] === x);
  }

  async function loadSavedHiscores() {
    const all = await idbGet("hiscores");
    if (!all || typeof all !== "object" || Array.isArray(all)) return {};
    return { ...all };
  }

  function hiscoreKey() { return String(programCrc >>> 0); }

  async function saveHiscoreNow(bytes) {
    const all = await loadSavedHiscores();
    all[hiscoreKey()] = bytes.map((x) => x & 0xff);
    await idbSet("hiscores", all);
    lastSavedHiscore = bytes.join(",");
  }

  let restoreInFlight = false;
  async function maybeRestoreHiscore() {
    if (!usingUserRom || restoreInFlight) return;
    const st = machine.state();
    if (!st.irq1Enable || st.frames < 60) return;
    if (!hiscoreIsFactory(readHiscore())) {
      hiscoreRestored = true;
      return;
    }
    restoreInFlight = true;
    try {
      const all = await loadSavedHiscores();
      const saved = all[hiscoreKey()];
      if (saved && saved.length >= HISCORE_LEN) writeHiscore(saved);
      hiscoreRestored = true;
      lastSavedHiscore = readHiscore().join(",");
    } finally {
      restoreInFlight = false;
    }
  }

  let saveInFlight = false;
  async function maybeSaveHiscore() {
    if (!usingUserRom || saveInFlight || !hiscoreRestored) return;
    const cur = readHiscore();
    const key = cur.join(",");
    if (key === lastSavedHiscore) return;
    saveInFlight = true;
    try { await saveHiscoreNow(cur); }
    finally { saveInFlight = false; }
  }

  async function resetHiscoreNow() {
    if (!usingUserRom) return;
    hiscoreRestored = false;
    lastSavedHiscore = readHiscore().join(",");
    const all = await loadSavedHiscores();
    delete all[hiscoreKey()];
    await idbSet("hiscores", all);
    machine.reset();
    applyKeys();
  }

  resetHiscore.addEventListener("click", () => {
    if (!usingUserRom) return;
    if (hiscoreResetHint && !hiscoreResetHint.open) hiscoreResetHint.showModal();
  });
  document.getElementById("hiscoreResetCancel")?.addEventListener("click", () => hiscoreResetHint?.close());
  document.getElementById("hiscoreResetOk")?.addEventListener("click", async () => {
    hiscoreResetHint?.close();
    await resetHiscoreNow();
  });

  function saveDips() {
    try {
      localStorage.setItem(DIP_KEY, JSON.stringify({
        credits: Number(dipCredits.value),
        difficulty: Number(dipDifficulty.value),
        demo: Number(dipDemo.value),
        freeze: Number(dipFreeze.value),
        rack: Number(dipRack.value),
        cabinet: Number(dipCabinet.value),
        coinage: Number(dipCoinage.value),
        bonus: Number(dipBonus.value),
        lives: Number(dipLives.value),
      }));
    } catch {}
    applyKeys();
  }

  function loadDips() {
    try {
      const raw = localStorage.getItem(DIP_KEY);
      if (raw) {
        const s = JSON.parse(raw);
        const map = {
          credits: dipCredits, difficulty: dipDifficulty, demo: dipDemo,
          freeze: dipFreeze, rack: dipRack, cabinet: dipCabinet,
          coinage: dipCoinage, bonus: dipBonus, lives: dipLives,
        };
        for (const [k, el] of Object.entries(map)) {
          if (s[k] !== undefined) el.value = String(s[k]);
        }
      }
    } catch {}
    applyKeys();
  }

  dipEls.forEach((el) => el.addEventListener("change", saveDips));
  loadDips();

  function orEmpty(bytes) { return bytes && bytes.length ? bytes : new Uint8Array(0); }

  function applySet(set, stored) {
    if (!set || !set.main || !set.sub || !set.sound || !set.tiles || !set.sprites ||
        !set.palette || !set.charLut || !set.spriteLut || !set.wave) {
      throw new Error("incomplete ROM set");
    }
    machine.loadRomSet(set.main, set.sub, set.sound, set.tiles, set.sprites,
      set.palette, set.charLut, set.spriteLut, set.wave,
      orEmpty(set.mcu51), orEmpty(set.mcu54));
    usingUserRom = set.kind !== "hwtest";
    programCrc = crc32(set.main);
    hiscoreRestored = false;
    lastSavedHiscore = "";
    syncHiscoreResetBtn();
    setStatus(statusFor(set, stored));
  }

  let hwtestCrcs = null;
  try { hwtestCrcs = await (await fetch("roms/crc.json")).json(); } catch {}

  function loadBuiltInRom() {
    machine.loadHwtest();
    usingUserRom = false;
    hiscoreRestored = false;
    lastSavedHiscore = "";
    syncHiscoreResetBtn();
    setStatus("Running test ROM");
  }

  const stored = await idbGet(ROM_KEY);
  if (stored) {
    try { applySet(stored, true); }
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
    await idbSet(ROM_KEY, set);
    applySet(set, false);
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
    applyKeys();
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
      mute,
      get muted() { return !!mute.checked; },
      readHiscore,
      writeHiscore,
      saveHiscoreNow,
      maybeSaveHiscore,
      resetHiscoreNow,
      restoreHiscoreNow: async () => {
        if (!usingUserRom) return false;
        const all = await loadSavedHiscores();
        const saved = all[hiscoreKey()];
        if (saved && saved.length >= HISCORE_LEN) writeHiscore(saved);
        hiscoreRestored = true;
        lastSavedHiscore = readHiscore().join(",");
        return true;
      },
      crc32,
      identifySet,
      hwtestCrcs,
      applySet,
      ROM_KEY,
    };
  }
});
