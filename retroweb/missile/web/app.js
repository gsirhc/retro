// Missile Command arcade front end. Guest CPU runs at the board average of 1.171875 MHz, paced to wall time.

const CPU_HZ = 1_171_875;
const IDB_NAME = "retroweb-missile";
const IDB_STORE = "roms";
const ROM_KEY = "set";
const TEST = new URLSearchParams(location.search).has("test");
const DIP_KEY = "retroweb.missile.dips";
const R8_FACTORY = 0x73; // 6 cities, 10000 bonus, mini trackball (MAME missile)
const R10_FACTORY = 0x81; // 2 coins 1 play, English
const HISCORE_ADDR = 0x2c;
const HISCORE_LEN = 48; // 24 initials, 24 BCD
const HISCORE_INITIALS = 0x24f3; // program offsets of the factory table
const HISCORE_SCORES = 0x250b;
const IN0_IDLE = 0xff;
const IN1_IDLE = 0x67;
const TRACK_STEP = 2;

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

function hexCrc(bytes) { return crc32(bytes).toString(16).padStart(8, "0"); }

function concat(parts, size) {
  const out = new Uint8Array(parts.length * size);
  parts.forEach((p, i) => out.set(p, i * size));
  return out;
}

// Board locations as named in MAME. 035826-01 is the optional L6 PROM.
const SIZE = { program: 0x3000, chip: 0x0800, prom: 0x20 };
const CHIPS = ["h1", "jk1", "kl1", "lm1", "np1", "r1"];
const NUM_RE = /(?:^|[._-])0358(2[0-6])(?:[^a-z0-9]|$)/i;
const LOC_RE = /(?:^|[._-])(h1|jk1|kl1|lm1|np1|r1|l6)(?:[^a-z0-9]|$)/i;
const NUM_LOC = { 20: "h1", 21: "jk1", 22: "kl1", 23: "lm1", 24: "np1", 25: "r1", 26: "l6" };

function clip(bytes, n) {
  if (!bytes || bytes.length < n) return null;
  return bytes.length === n ? bytes : bytes.subarray(0, n);
}

function roleOf(name) {
  const n = String(name).toLowerCase().split("/").pop();
  if (n === "program.bin" || n === "prg.bin") return "program";
  if (n === "prom.bin") return "l6";
  const m = n.match(NUM_RE);
  if (m) return NUM_LOC[m[1]];
  const l = n.match(LOC_RE);
  return l ? l[1].toLowerCase() : null;
}

function identifySet(files, hwtestCrcs) {
  const found = {};
  for (const [name, data] of Object.entries(files)) {
    const role = roleOf(name);
    if (!role || found[role]) continue;
    found[role] = data;
  }
  let program = clip(found.program, SIZE.program);
  if (!program) {
    const chips = CHIPS.map((c) => clip(found[c], SIZE.chip));
    if (chips.every(Boolean)) program = concat(chips, SIZE.chip);
  }
  if (!program) return null;
  let prom = null;
  if (found.l6) {
    prom = clip(found.l6, SIZE.prom);
    if (!prom) return null;
  }
  const kind = hwtestCrcs && hexCrc(program) === hwtestCrcs.program ? "hwtest" : "user";
  return { kind, program, prom };
}

function labelFor(kind) {
  if (kind === "hwtest") return "Hardware test ROM";
  return "Loaded ROM set";
}

initThemePicker();
initFullscreen({
  bezelEl: document.getElementById("bezel"),
  screenEl: document.getElementById("screen"),
  fullscreenBtn: document.getElementById("fullscreenBtn"),
  isRunning: () => true,
});
// Nothing focuses the screen on load, so nudge the hint once up front.
const updateFocusHint = initFocusHint(document.getElementById("screen"), () => true);
updateFocusHint();

MissileArcade().then(async (Module) => {
  const machine = new Module.Machine();
  const screen = document.getElementById("screen");
  const ctx = screen.getContext("2d");
  const img = ctx.createImageData(machine.screenWidth(), machine.screenHeight());
  const status = document.getElementById("romStatus");
  const mute = document.getElementById("mute");
  function isMuted() { return mute.getAttribute("aria-pressed") === "true"; }
  mute.addEventListener("click", () => {
    const next = !isMuted();
    mute.setAttribute("aria-pressed", next ? "true" : "false");
    mute.title = next ? "Unmute" : "Mute";
    mute.setAttribute("aria-label", next ? "Unmute" : "Mute");
  });
  const resetHiscore = document.getElementById("resetHiscore");
  const hiscoreResetHint = document.getElementById("hiscoreResetHint");

  let usingUserRom = false;
  let program = null;
  let romKey = 0;
  let hiscoreRestored = false;
  let lastSavedHiscore = "";
  let selfTest = false;
  const keys = {};
  const coinUntil = { 1: 0, 2: 0 };
  const coinDoor = document.getElementById("coinDoor");
  const track = { x: 0, y: 0 };

  function blit() {
    img.data.set(machine.frameBuffer());
    ctx.putImageData(img, 0, 0);
  }

  // IN0 and IN1 are active-low.
  function applyKeys() {
    let n0 = IN0_IDLE;
    let n1 = IN1_IDLE;
    const down = (k) => keys[k];
    const now = Date.now();
    if (down("Digit1") || down("Numpad1")) n0 &= ~0x10;
    if (down("Digit2") || down("Numpad2")) n0 &= ~0x08;
    if (down("Digit5") || down("Numpad5") || now < coinUntil[1]) n0 &= ~0x20;
    if (down("Digit6") || down("Numpad6") || now < coinUntil[2]) n0 &= ~0x80;
    if (down("KeyZ") || down("Mouse0")) n1 &= ~0x04;
    if (down("KeyX") || down("Mouse1")) n1 &= ~0x02;
    if (down("KeyC") || down("Mouse2")) n1 &= ~0x01;
    if (down("KeyT")) n1 &= ~0x20;
    if (selfTest) n1 &= ~0x40;
    machine.setIn0(n0);
    machine.setIn1(n1);
  }

  // Mouse deltas accumulated between frames (pointer-lock or drag on #screen).
  let mouseDx = 0, mouseDy = 0;

  // Two 4-bit counters, read as VVVVHHHH. Y is wired reversed (MAME
  // TRACK0_Y PORT_REVERSE), so rolling down counts down.
  function nudgeTrack(dx, dy) {
    track.x = (track.x + dx) & 0x0f;
    track.y = (track.y + dy) & 0x0f;
    machine.setTrackX(track.x);
    machine.setTrackY(track.y);
  }

  function stepTrackball() {
    const kdx = ((keys.ArrowRight || keys.KeyD) ? 1 : 0) - ((keys.ArrowLeft || keys.KeyA) ? 1 : 0);
    const kdy = ((keys.ArrowDown || keys.KeyS) ? 1 : 0) - ((keys.ArrowUp || keys.KeyW) ? 1 : 0);
    // Scale mouse pixels into a few counter ticks per frame.
    const mdx = Math.max(-7, Math.min(7, Math.round(mouseDx / 4)));
    const mdy = Math.max(-7, Math.min(7, Math.round(mouseDy / 4)));
    mouseDx = 0;
    mouseDy = 0;
    const dx = kdx * TRACK_STEP + mdx;
    const dy = kdy * TRACK_STEP + mdy;
    if (dx || dy) nudgeTrack(dx, -dy);
  }

  function pointerLocked() {
    return document.pointerLockElement === screen;
  }

  function releaseMouseButtons() {
    keys.Mouse0 = keys.Mouse1 = keys.Mouse2 = false;
    applyKeys();
  }

  screen.addEventListener("click", () => {
    if (!pointerLocked()) screen.requestPointerLock?.();
  });
  document.addEventListener("pointerlockchange", () => {
    if (pointerLocked()) screen.focus();
    else releaseMouseButtons();
  });
  screen.addEventListener("contextmenu", (e) => e.preventDefault());
  screen.addEventListener("mousemove", (e) => {
    if (pointerLocked()) {
      mouseDx += e.movementX;
      mouseDy += e.movementY;
      return;
    }
    // Drag without lock still rolls the ball (touchpads / denied lock).
    if (e.buttons & 1) {
      mouseDx += e.movementX;
      mouseDy += e.movementY;
    }
  });
  screen.addEventListener("mousedown", (e) => {
    if (!pointerLocked() || e.button > 2) return;
    e.preventDefault();
    keys["Mouse" + e.button] = true;
    applyKeys();
  });
  window.addEventListener("mouseup", (e) => {
    if (e.button > 2 || !keys["Mouse" + e.button]) return;
    keys["Mouse" + e.button] = false;
    applyKeys();
  });

  function insertCoin(which) {
    coinUntil[which] = Date.now() + 120;
    applyKeys();
    window.setTimeout(applyKeys, 130);
    coinDoor.classList.add("coined");
    window.setTimeout(() => coinDoor.classList.remove("coined"), 180);
    screen.focus();
  }
  coinDoor.querySelectorAll("[data-coin]").forEach((el) => {
    el.addEventListener("click", () => insertCoin(el.getAttribute("data-coin") === "2" ? 2 : 1));
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

  const touch = initArcadeTouch({
    root: document.getElementById("touchControls"),
    stick: "8way",
    buttons: [
      { code: "KeyZ", label: "Alpha", cls: "red" },
      { code: "KeyX", label: "Delta", cls: "red" },
      { code: "KeyC", label: "Omega", cls: "red" },
    ],
    setKey: (code, down) => {
      keys[code] = down;
      applyKeys();
    },
  });

  window.addEventListener("keydown", (e) => {
    keys[e.code] = true;
    applyKeys();
    if (["ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "Space"].includes(e.code)) e.preventDefault();
  });
  window.addEventListener("keyup", (e) => {
    keys[e.code] = false;
    applyKeys();
  });

  let audioCtx = null, audioNode = null;
  async function ensureAudio() {
    if (isMuted()) return;
    if (audioCtx) {
      if (audioCtx.state === "suspended") await audioCtx.resume();
      return;
    }
    audioCtx = new (window.AudioContext || window.webkitAudioContext)({ latencyHint: "interactive" });
    // Output device rate varies, don't assume 48 kHz.
    machine.setAudioHz(audioCtx.sampleRate);
    if (!audioCtx.audioWorklet) return;
    const src = `registerProcessor("msl", class extends AudioWorkletProcessor {
      constructor() {
        super();
        this.q = [];
        this.i = 0;
        this.port.onmessage = e => {
          // Drop backlog so a main-thread stall cannot leave the sound behind the picture.
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
    audioNode = new AudioWorkletNode(audioCtx, "msl");
    audioNode.connect(audioCtx.destination);
  }

  document.addEventListener("click", () => { ensureAudio().catch(() => {}); }, { once: true });

  function pumpAudio() {
    if (!audioNode || isMuted()) {
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
    for (let i = 0; i < HISCORE_LEN; i++) b.push(machine.ramByte(HISCORE_ADDR + i) & 0xff);
    return b;
  }

  function writeHiscore(bytes) {
    if (!bytes || bytes.length < HISCORE_LEN) return;
    for (let i = 0; i < HISCORE_LEN; i++) machine.setRamByte(HISCORE_ADDR + i, bytes[i] & 0xff);
  }

  // The ROM copies its own default table into RAM after clearing zero page.
  function factoryHiscore() {
    if (!program) return null;
    return [
      ...program.subarray(HISCORE_INITIALS, HISCORE_INITIALS + 24),
      ...program.subarray(HISCORE_SCORES, HISCORE_SCORES + 24),
    ];
  }

  function hiscoreIsFactory(b) {
    const f = factoryHiscore();
    return !!f && b.every((v, i) => v === f[i]);
  }

  async function loadSavedHiscores() {
    const all = await idbGet("hiscores");
    if (!all || typeof all !== "object" || Array.isArray(all)) return {};
    return { ...all };
  }

  function hiscoreKey() { return String(romKey >>> 0); }

  async function saveHiscoreNow(bytes) {
    const all = await loadSavedHiscores();
    all[hiscoreKey()] = Array.from(bytes).slice(0, HISCORE_LEN);
    await idbSet("hiscores", all);
    lastSavedHiscore = bytes.join(",");
  }

  let restoreInFlight = false;
  async function maybeRestoreHiscore() {
    if (!usingUserRom || restoreInFlight || hiscoreRestored) return;
    if (!hiscoreIsFactory(readHiscore())) return;
    restoreInFlight = true;
    const key = hiscoreKey();
    try {
      const all = await loadSavedHiscores();
      if (key !== hiscoreKey() || hiscoreRestored) return;
      const saved = all[key];
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
    if (hiscoreIsFactory(cur)) return;
    const key = cur.join(",");
    if (key === lastSavedHiscore) return;
    saveInFlight = true;
    try { await saveHiscoreNow(cur); }
    finally { saveInFlight = false; }
  }

  async function resetHiscoreNow() {
    if (!usingUserRom) return;
    hiscoreRestored = false;
    const all = await loadSavedHiscores();
    delete all[hiscoreKey()];
    await idbSet("hiscores", all);
    machine.reset();
    lastSavedHiscore = "";
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

  const dipIds = ["dipCities", "dipBonusCredit", "dipTrackball", "dipBonusCity", "dipCabinet"];
  const dipR10Ids = ["dipCoinage", "dipRightMult", "dipCenterMult", "dipLanguage", "dipSpare"];
  const dipEls = Object.fromEntries([...dipIds, ...dipR10Ids].map((id) => [id, document.getElementById(id)]));
  const dipSelfTest = document.getElementById("dipSelfTest");

  const sumOf = (ids) => ids.reduce((n, id) => n | Number(dipEls[id].value), 0) & 0xff;
  const encodeR8 = () => sumOf(dipIds);
  const encodeR10 = () => sumOf(dipR10Ids);

  function applyDips() {
    machine.setR8(encodeR8());
    machine.setR10(encodeR10());
    selfTest = !!dipSelfTest.checked;
    applyKeys();
  }

  function saveDips() {
    try {
      localStorage.setItem(DIP_KEY, JSON.stringify({
        r8: encodeR8(),
        r10: encodeR10(),
        selfTest: !!dipSelfTest.checked,
      }));
    } catch {}
    applyDips();
  }

  const R8_MASKS = { dipCities: 0x03, dipBonusCredit: 0x04, dipTrackball: 0x08, dipBonusCity: 0x70, dipCabinet: 0x80 };
  const R10_MASKS = { dipCoinage: 0x03, dipRightMult: 0x0c, dipCenterMult: 0x10, dipLanguage: 0x60, dipSpare: 0x80 };

  function loadDips() {
    let r8 = R8_FACTORY;
    let r10 = R10_FACTORY;
    try {
      const raw = localStorage.getItem(DIP_KEY);
      if (raw) {
        const s = JSON.parse(raw);
        if (typeof s.r8 === "number") r8 = s.r8 & 0xff;
        if (typeof s.r10 === "number") r10 = s.r10 & 0xff;
        dipSelfTest.checked = !!s.selfTest;
      }
    } catch {}
    for (const [id, mask] of Object.entries(R8_MASKS)) dipEls[id].value = String(r8 & mask);
    for (const [id, mask] of Object.entries(R10_MASKS)) dipEls[id].value = String(r10 & mask);
    applyDips();
  }

  [...Object.values(dipEls), dipSelfTest].forEach((el) => el.addEventListener("change", saveDips));
  loadDips();

  async function applySet(set, label) {
    if (!set || !set.program) throw new Error("incomplete ROM set");
    machine.loadRomSet(set.program, set.prom || null);
    program = set.program;
    usingUserRom = set.kind !== "hwtest";
    romKey = crc32(set.program);
    hiscoreRestored = false;
    lastSavedHiscore = "";
    syncHiscoreResetBtn();
    setStatus(label);
  }

  let hwtestCrcs = null;
  try {
    const c = await (await fetch("roms/crc.json")).json();
    hwtestCrcs = { program: c.program };
  } catch {}

  function loadBuiltInRom() {
    machine.loadHwtest();
    program = null;
    usingUserRom = false;
    hiscoreRestored = false;
    lastSavedHiscore = "";
    syncHiscoreResetBtn();
    setStatus("Running test ROM");
  }

  let stored = null;
  try { stored = await idbGet(ROM_KEY); } catch {}
  if (stored) {
    try { await applySet(stored, "Loaded stored ROM set"); }
    catch (e) {
      loadBuiltInRom();
      setStatus("Stored ROM unreadable. Using test ROM");
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
      const bytes = f.bytes instanceof Uint8Array
        ? f.bytes
        : new Uint8Array(await f.arrayBuffer());
      if (name.endsWith(".zip")) Object.assign(files, await unzip(bufOf(bytes)));
      else files[name] = bytes;
    }
    const set = identifySet(files, hwtestCrcs);
    if (!set) throw new Error("incomplete or wrong-sized ROM set");
    await applySet(set, labelFor(set.kind));
    try { await idbSet(ROM_KEY, set); } catch { setStatus("Warning: Your ROM has been loaded but the browser blocked storage (IndexedDB).  It won't reload when you return."); }
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

  document.getElementById("removeRomBtn").addEventListener("click", async () => {
    try { await idbDel(ROM_KEY); } catch {}
    loadBuiltInRom();
  });

  let lastT = null;
  let trackAcc = 0;
  function tick(t) {
    if (lastT === null) lastT = t;
    let dt = (t - lastT) / 1000;
    lastT = t;
    if (dt > 0.08) dt = 0.08;
    trackAcc += dt;
    while (trackAcc >= 1 / 60) {
      trackAcc -= 1 / 60;
      stepTrackball();
    }
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
      touch,
      get status() { return status.textContent; },
      get usingUserRom() { return usingUserRom; },
      mute,
      get muted() { return isMuted(); },
      encodeR8,
      encodeR10,
      get trackPos() { return { x: track.x, y: track.y }; },
      readHiscore,
      writeHiscore,
      factoryHiscore,
      saveHiscoreNow,
      maybeSaveHiscore,
      resetHiscoreNow,
      get hiscoreRestored() { return hiscoreRestored; },
      get hiscoreKey() { return hiscoreKey(); },
      crc32,
      identifySet,
      hwtestCrcs,
      applySet,
      ROM_KEY,
      loadSavedHiscores,
      wipeStorage: async () => {
        await idbDel("set");
        await idbDel("hiscores");
      },
    };
  }
});
