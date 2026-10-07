// Asteroids arcade front end. Guest CPU is always 1.512 MHz vs wall clock.

const CPU_HZ = 1_512_000;
const IDB_NAME = "retroweb-asteroids";
const IDB_STORE = "roms";
const ROM_KEY = "set";
const TEST = new URLSearchParams(location.search).has("test");
const DIP_KEY = "retroweb.asteroids.dips";
const DSW1_FACTORY = 0x12; // 1C/1C, x1 center, 3 lives (bit4), English
// $2802 pair: 0=1x&4 lives, 1=1x&3, 2=2x&4, 3=2x&3 (computerarcheology).
// Work RAM range the real board's high-score table lives in (through the
// initials + score entries at $51). See .claude/arcade.md "High scores" --
// persist this range for a user ROM, never invent an initials overlay.
const HISCORE_ADDR = 0x1d;
const HISCORE_LEN = 0x35;

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

function concat3(parts) {
  const out = new Uint8Array(0x1800);
  for (let i = 0; i < 3; i++) out.set(parts[i], i * 0x800);
  return out;
}

// Board locations on the original Atari PCB, as they appear in MAME names
// (035145-04e.ef2, 035144-04e.h2, 035143-02.j2, 035127-02.np3). 034602 is
// the bonus-coin PROM on later revisions; this core doesn't implement its
// coin-counter encoding, so loose dumps that include it are just ignored.
const SIZE = { program: 0x1800, chip: 0x0800, vector: 0x0800 };
const CHIP_RE = /(?:^|[._-])(035145|ef2|035144|h2|035143|j2|035127|np3)(?:[^a-z0-9]|$)/i;
const IGNORE_RE = /(?:^|[._-])(034602)(?:[^a-z0-9]|$)/i;
const ROLE_MAP = {
  "035145": "ef2", ef2: "ef2",
  "035144": "h2", h2: "h2",
  "035143": "j2", j2: "j2",
  "035127": "np3", np3: "np3",
};

function clip(bytes, n) {
  if (!bytes || bytes.length < n) return null;
  return bytes.length === n ? bytes : bytes.subarray(0, n);
}

function roleOf(name) {
  const n = String(name).toLowerCase().split("/").pop();
  if (n === "program.bin" || n === "prg.bin") return "program";
  if (n === "vector.bin" || n === "vec.bin") return "vector";
  if (IGNORE_RE.test(n)) return "ignore";
  const m = n.match(CHIP_RE);
  return m ? ROLE_MAP[m[1].toLowerCase()] : null;
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
    const chips = ["ef2", "h2", "j2"].map((c) => clip(found[c], SIZE.chip));
    if (chips.every(Boolean)) program = concat3(chips);
  }
  let vector = clip(found.vector, SIZE.vector);
  if (!vector) vector = clip(found.np3, SIZE.vector);
  if (!program || !vector) return null;

  let kind = "user";
  if (hwtestCrcs &&
      hexCrc(program) === hwtestCrcs.program &&
      hexCrc(vector) === hwtestCrcs.vector) {
    kind = "hwtest";
  }
  return { kind, program, vector };
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
// initFocusHint only updates the banner on later focusin/focusout events --
// call it once now too, since nothing here calls screen.focus() on load the
// way altair8800/assembler6502's terminal-focusing boot flow does, so the
// hint needs an explicit nudge to reflect "nothing is focused yet" from the
// very first frame (same reasoning as ibmpc-at's own post-power-on call).
const updateFocusHint = initFocusHint(document.getElementById("screen"), () => true);
updateFocusHint();

AsteroidsArcade().then(async (Module) => {
  const machine = new Module.Machine();
  const screen = document.getElementById("screen");
  const ctx = screen.getContext("2d", { alpha: false });
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
  let romKey = 0;
  let hiscoreRestored = false;
  let lastSavedHiscore = "";
  let selfTest = false;
  const keys = {};
  const coinUntil = { left: 0, center: 0, right: 0 };
  const coinDoor = document.getElementById("coinDoor");

  // Stroke vectors at the canvas's device-pixel size so line weight tracks
  // CSS/fullscreen scale (a fixed 1024² blit goes soft under bilinear).
  function syncCanvasBacking() {
    const dpr = window.devicePixelRatio || 1;
    const css = screen.getBoundingClientRect();
    const w = Math.max(1, Math.round(css.width * dpr));
    const h = Math.max(1, Math.round(css.height * dpr));
    if (screen.width !== w || screen.height !== h) {
      screen.width = w;
      screen.height = h;
    }
    return { w, h, dpr };
  }

  function blit() {
    const { w, h, dpr } = syncCanvasBacking();
    const segs = machine.vectorSegments();
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.fillStyle = "#000";
    ctx.fillRect(0, 0, w, h);
    // 4:3 tube. The attract picture uses about beam Y 96..927 (scores
    // at the top, copyright at the bottom), so the height is opened to
    // 64..960 and that span fills the glass. X stays the full 0..1023.
    const sx = w / 1024;
    const sy = h / 896;
    const yTop = 960;
    ctx.lineCap = "round";
    ctx.lineJoin = "round";
    const glowW = Math.max(2, 2.2 * dpr);
    const coreW = Math.max(1.1, 1.15 * dpr);
    const spotR = Math.max(1.2, 1.4 * dpr);
    for (let pass = 0; pass < 2; pass++) {
      const glow = pass === 0;
      ctx.lineWidth = glow ? glowW : coreW;
      for (let i = 0; i + 4 < segs.length; i += 5) {
        const x0 = segs[i] * sx;
        const y0 = (yTop - segs[i + 1]) * sy;
        const x1 = segs[i + 2] * sx;
        const y1 = (yTop - segs[i + 3]) * sy;
        const bri = segs[i + 4];
        const a = glow ? 0.18 + bri * 0.02 : 0.45 + bri * 0.036;
        const c = glow ? 160 + bri * 4 : 200 + bri * 3;
        const col = `rgba(${c|0},${c|0},${Math.min(255, c + 20)|0},${Math.min(1, a)})`;
        // Zero-length bright VEC = photon shot (XY spot).
        if (Math.abs(x1 - x0) < 0.5 && Math.abs(y1 - y0) < 0.5) {
          ctx.fillStyle = col;
          ctx.beginPath();
          ctx.arc(x0, y0, glow ? spotR * 1.8 : spotR, 0, Math.PI * 2);
          ctx.fill();
          continue;
        }
        ctx.strokeStyle = col;
        ctx.beginPath();
        ctx.moveTo(x0, y0);
        ctx.lineTo(x1, y1);
        ctx.stroke();
      }
    }
  }

  // Inputs are active-high as the board's own open-collector latch sees
  // them (1 = pressed) -- see Inputs in machine.h, unlike the active-low
  // edge connectors on Pac-Man/Galaxian-family boards.
  function applyKeys() {
    let n0 = 0, n1 = 0;
    const down = (k) => keys[k];
    if (down("ShiftLeft") || down("ShiftRight")) n0 |= 0x08;   // hyperspace
    if (down("KeyZ") || down("Space")) n0 |= 0x10; // fire
    if (selfTest) n0 |= 0x80;
    if (down("Digit5") || down("Numpad5") || Date.now() < coinUntil.left) n1 |= 0x01;
    if (down("Digit6") || down("Numpad6") || Date.now() < coinUntil.center) n1 |= 0x02;
    if (down("Digit7") || down("Numpad7") || Date.now() < coinUntil.right) n1 |= 0x04;
    if (down("Digit1") || down("Numpad1")) n1 |= 0x08; // 1P start
    if (down("Digit2") || down("Numpad2")) n1 |= 0x10; // 2P start
    if (down("ArrowUp") || down("KeyW")) n1 |= 0x20;   // thrust
    if (down("ArrowRight") || down("KeyD")) n1 |= 0x40; // rotate right
    if (down("ArrowLeft") || down("KeyA")) n1 |= 0x80;  // rotate left
    machine.setIn0(n0);
    machine.setIn1(n1);
  }

  function insertCoin() {
    // Both slots are the left coin (IN1 bit 0), same as the 5 key.
    // Center and right mechs stay on 6 and 7.
    coinUntil.left = Date.now() + 120;
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
    audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    // The discrete-sound mixer's cycle-rate sample generation resamples to
    // whatever the actual output device rate is -- don't assume 48 kHz.
    machine.setAudioHz(audioCtx.sampleRate);
    if (!audioCtx.audioWorklet) return;
    const src = `registerProcessor("ast", class extends AudioWorkletProcessor {
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
    audioNode = new AudioWorkletNode(audioCtx, "ast");
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

  // Scores are $1D–$30 (20 bytes). $31–$33 are live initials-entry
  // state: CheckHighScore writes $FF into the rank bytes on attract
  // (NumPlayers = $FF). Those must not look like a filled table, or
  // restore never arms and we never save. Cite: 6502disassembly.com
  // va-asteroids CheckHighScore $7664.
  function hiscoreIsFactory(b) {
    for (let i = 0; i < 20; i++) if (b[i]) return false;
    return true;
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

  // service_nmi() runs at ≈246 Hz and increments frames; reset() zeroes both
  // frames and work RAM, so a few hundred frames is comfortably past the
  // real ROM's own startup RAM clear -- restoring any earlier is racing
  // the game's own POST wipe (see .claude/arcade.md "Restore after POST").
  function postDone() {
    return machine.frames() >= 180 && hiscoreIsFactory(readHiscore());
  }

  let restoreInFlight = false;
  async function maybeRestoreHiscore() {
    if (!usingUserRom || restoreInFlight || hiscoreRestored) return;
    if (!postDone()) return;
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
    lastSavedHiscore = readHiscore().join(",");
    const all = await loadSavedHiscores();
    delete all[hiscoreKey()];
    await idbSet("hiscores", all);
    machine.reset();
    applyDips();
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

  const dipCoinage = document.getElementById("dipCoinage");
  const dipRightMech = document.getElementById("dipRightMech");
  const dipCenterMech = document.getElementById("dipCenterMech");
  const dipLives = document.getElementById("dipLives");
  const dipLanguage = document.getElementById("dipLanguage");
  const dipSelfTest = document.getElementById("dipSelfTest");

  function encodeDsw1() {
    return (Number(dipCoinage.value) | Number(dipRightMech.value) | Number(dipCenterMech.value) |
            Number(dipLives.value) | Number(dipLanguage.value)) & 0xff;
  }

  function applyDips() {
    machine.setDsw1(encodeDsw1());
    selfTest = !!dipSelfTest.checked;
    applyKeys();
  }

  function saveDips() {
    try {
      localStorage.setItem(DIP_KEY, JSON.stringify({
        dsw1: encodeDsw1(),
        selfTest: !!dipSelfTest.checked,
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
        dipSelfTest.checked = !!s.selfTest;
      }
    } catch {}
    dipCoinage.value = String(dsw1 & 0x03);
    dipRightMech.value = String(dsw1 & 0x0c);
    dipCenterMech.value = String(dsw1 & 0x20);
    dipLives.value = String(dsw1 & 0x10);
    dipLanguage.value = String(dsw1 & 0xc0);
    applyDips();
  }

  [dipCoinage, dipRightMech, dipCenterMech, dipLives, dipLanguage, dipSelfTest]
    .forEach((el) => el.addEventListener("change", saveDips));
  loadDips();

  function applySet(set, label) {
    if (!set || !set.program || !set.vector) throw new Error("incomplete ROM set");
    machine.loadRomSet(set.program, set.vector);
    usingUserRom = set.kind !== "hwtest";
    romKey = crc32(set.program) ^ crc32(set.vector);
    hiscoreRestored = false;
    lastSavedHiscore = "";
    syncHiscoreResetBtn();
    setStatus(label);
  }

  let hwtestCrcs = null;
  try {
    const c = await (await fetch("roms/crc.json")).json();
    hwtestCrcs = { program: c.program, vector: c.vector };
  } catch {}

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
    try { applySet(stored, "Loaded stored ROM set"); }
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
      mute,
      get muted() { return isMuted(); },
      encodeDsw1,
      readHiscore,
      writeHiscore,
      saveHiscoreNow,
      maybeSaveHiscore,
      resetHiscoreNow,
      get hiscoreRestored() { return hiscoreRestored; },
      get hiscoreKey() { return hiscoreKey(); },
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
      loadSavedHiscores,
      wipeStorage: async () => {
        await idbDel("set");
        await idbDel("hiscores");
      },
    };
  }
});
