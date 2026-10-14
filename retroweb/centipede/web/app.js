// Centipede arcade front end. Guest CPU is always 1.512 MHz vs wall clock.

const CPU_HZ = 1_512_000;
const IDB_NAME = "retroweb-centipede";
const IDB_STORE = "roms";
const ROM_KEY = "set";
const TEST = new URLSearchParams(location.search).has("test");
const DIP_KEY = "retroweb.centipede.dips";
const DSW1_FACTORY = 0x54; // 3 lives, 12000 bonus, easy, English (MAME centiped)
const DSW2_FACTORY = 0x02; // 1C/1C
const HISCORE_LEN = 0x40; // ER2055 is 64 x 8
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

// Board locations as named in MAME. 213/214 are the colour PROMs, which
// this core doesn't read, so dumps that include them are ignored.
const SIZE = { program: 0x2000, gfx: 0x1000, chip: 0x0800 };
// Program chips: rev 3 is 307–310, rev 4 (common MAME centiped set) is 407–410.
const NUM_RE = /(?:^|[._-])(307|308|309|310|407|408|409|410|211|212)(?:[^a-z0-9]|$)/i;
const LOC_RE = /(?:^|[._-])(d1|e1|fh1|j1|f7|hj7)(?:[^a-z0-9]|$)/i;
const IGNORE_RE = /(?:^|[._-])(213|214)(?:[^a-z0-9]|$)/i;
const LOC_NUM = { d1: "307", e1: "308", fh1: "309", j1: "310", f7: "211", hj7: "212" };
const NUM_ALIAS = { "407": "307", "408": "308", "409": "309", "410": "310" };
const PROGRAM_CHIPS = ["307", "308", "309", "310"];
const GFX_CHIPS = ["211", "212"];

function clip(bytes, n) {
  if (!bytes || bytes.length < n) return null;
  return bytes.length === n ? bytes : bytes.subarray(0, n);
}

function roleOf(name) {
  const n = String(name).toLowerCase().split("/").pop();
  if (n === "program.bin" || n === "prg.bin") return "program";
  if (n === "gfx.bin" || n === "gfx") return "gfx";
  if (IGNORE_RE.test(n)) return "ignore";
  const m = n.match(NUM_RE);
  if (m) return NUM_ALIAS[m[1]] || m[1];
  const l = n.match(LOC_RE);
  return l ? LOC_NUM[l[1].toLowerCase()] : null;
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
    const chips = PROGRAM_CHIPS.map((c) => clip(found[c], SIZE.chip));
    if (chips.every(Boolean)) program = concat(chips, SIZE.chip);
  }
  let gfx = clip(found.gfx, SIZE.gfx);
  if (!gfx) {
    const chips = GFX_CHIPS.map((c) => clip(found[c], SIZE.chip));
    if (chips.every(Boolean)) gfx = concat(chips, SIZE.chip);
  }
  if (!program || !gfx) return null;

  let kind = "user";
  if (hwtestCrcs &&
      hexCrc(program) === hwtestCrcs.program &&
      hexCrc(gfx) === hwtestCrcs.gfx) {
    kind = "hwtest";
  }
  return { kind, program, gfx };
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

CentipedeArcade().then(async (Module) => {
  const machine = new Module.Machine();
  const screen = document.getElementById("screen");
  const ctx = screen.getContext("2d");
  const img = ctx.createImageData(240, 256);
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
  const coinUntil = { 1: 0, 2: 0 };
  const coinDoor = document.getElementById("coinDoor");
  const track = { x: 0, y: 0, signX: 0, signY: 0 };

  function blit() {
    img.data.set(machine.frameBuffer());
    ctx.putImageData(img, 0, 0);
  }

  // IN1 is active-low, idle 0xFF.
  function applyKeys() {
    let n1 = 0xff;
    const down = (k) => keys[k];
    if (down("Digit1") || down("Numpad1")) n1 &= ~0x01;
    if (down("Digit2") || down("Numpad2")) n1 &= ~0x02;
    if (down("KeyZ") || down("Space")) n1 &= ~0x04;
    if (down("Digit5") || down("Numpad5") || Date.now() < coinUntil[1]) n1 &= ~0x20;
    if (down("Digit6") || down("Numpad6") || Date.now() < coinUntil[2]) n1 &= ~0x40;
    machine.setIn1(n1);
    machine.setIn0Switches(selfTest ? 0x00 : 0x20);
  }

  // Mouse deltas accumulated between frames (pointer-lock or drag on #screen).
  let mouseDx = 0, mouseDy = 0;

  // The 4-bit counter holds its value between pushes and the sign latches the
  // last direction (MAME centiped.cpp read_trackball). X is wired reversed
  // (TRACK0_X PORT_REVERSE), so right counts down.
  function nudgeTrack(dx, dy) {
    if (dx) {
      track.x = (track.x + dx) & 0xff;
      track.signX = dx < 0 ? 0x80 : 0;
    }
    if (dy) {
      track.y = (track.y + dy) & 0xff;
      track.signY = dy < 0 ? 0x80 : 0;
    }
    machine.setTrackX(track.signX | (track.x & 0x0f));
    machine.setTrackY(track.signY | (track.y & 0x0f));
  }

  function stepTrackball() {
    const kdx = ((keys.ArrowLeft || keys.KeyA) ? 1 : 0) - ((keys.ArrowRight || keys.KeyD) ? 1 : 0);
    const kdy = ((keys.ArrowDown || keys.KeyS) ? 1 : 0) - ((keys.ArrowUp || keys.KeyW) ? 1 : 0);
    if (kdx || kdy) nudgeTrack(kdx * TRACK_STEP, kdy * TRACK_STEP);
    // Scale mouse pixels into a few counter ticks per frame.
    const mdx = Math.max(-7, Math.min(7, Math.round(mouseDx / 4)));
    const mdy = Math.max(-7, Math.min(7, Math.round(mouseDy / 4)));
    mouseDx = 0;
    mouseDy = 0;
    // Same PORT_REVERSE on X as the keyboard path.
    if (mdx || mdy) nudgeTrack(-mdx, mdy);
  }

  function pointerLocked() {
    return document.pointerLockElement === screen;
  }

  screen.addEventListener("click", () => {
    if (!pointerLocked()) screen.requestPointerLock?.();
  });
  document.addEventListener("pointerlockchange", () => {
    if (pointerLocked()) screen.focus();
  });
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
    buttons: [{ code: "Space", label: "Fire", cls: "red" }],
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
    const src = `registerProcessor("cen", class extends AudioWorkletProcessor {
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
    audioNode = new AudioWorkletNode(audioCtx, "cen");
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
    return Array.from(machine.earomBytes());
  }

  function writeHiscore(bytes) {
    if (!bytes || bytes.length < HISCORE_LEN) return;
    for (let i = 0; i < HISCORE_LEN; i++) machine.setEaromByte(i, bytes[i] & 0xff);
  }

  function hiscoreIsFactory(b) {
    for (let i = 0; i < b.length; i++) if (b[i]) return false;
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

  // The EAROM holds its contents with the power off, so the saved image goes
  // in before the game reads it, then the board restarts.
  async function restoreHiscoreNow() {
    if (!usingUserRom) return false;
    const key = hiscoreKey();
    const all = await loadSavedHiscores();
    if (key !== hiscoreKey()) return false;
    const saved = all[key];
    if (saved && saved.length >= HISCORE_LEN) {
      writeHiscore(saved);
      machine.reset();
    }
    hiscoreRestored = true;
    lastSavedHiscore = readHiscore().join(",");
    return true;
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
    writeHiscore(new Array(HISCORE_LEN).fill(0));
    lastSavedHiscore = readHiscore().join(",");
    machine.reset();
    applyDips();
    hiscoreRestored = true;
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

  const dipLives = document.getElementById("dipLives");
  const dipBonus = document.getElementById("dipBonus");
  const dipDifficulty = document.getElementById("dipDifficulty");
  const dipLanguage = document.getElementById("dipLanguage");
  const dipCoinage = document.getElementById("dipCoinage");
  const dipSelfTest = document.getElementById("dipSelfTest");

  function encodeDsw1() {
    return (Number(dipLives.value) | Number(dipBonus.value) | Number(dipDifficulty.value) |
            Number(dipLanguage.value)) & 0xff;
  }

  function encodeDsw2() {
    return Number(dipCoinage.value) & 0xff;
  }

  function applyDips() {
    machine.setDsw1(encodeDsw1());
    machine.setDsw2(encodeDsw2());
    selfTest = !!dipSelfTest.checked;
    applyKeys();
  }

  function saveDips() {
    try {
      localStorage.setItem(DIP_KEY, JSON.stringify({
        dsw1: encodeDsw1(),
        dsw2: encodeDsw2(),
        selfTest: !!dipSelfTest.checked,
      }));
    } catch {}
    applyDips();
  }

  function loadDips() {
    let dsw1 = DSW1_FACTORY;
    let dsw2 = DSW2_FACTORY;
    try {
      const raw = localStorage.getItem(DIP_KEY);
      if (raw) {
        const s = JSON.parse(raw);
        if (typeof s.dsw1 === "number") dsw1 = s.dsw1 & 0xff;
        if (typeof s.dsw2 === "number") dsw2 = s.dsw2 & 0xff;
        dipSelfTest.checked = !!s.selfTest;
      }
    } catch {}
    dipLives.value = String(dsw1 & 0x0c);
    dipBonus.value = String(dsw1 & 0x30);
    dipDifficulty.value = String(dsw1 & 0x40);
    dipLanguage.value = String(dsw1 & 0x03);
    dipCoinage.value = String(dsw2 & 0x03);
    applyDips();
  }

  [dipLives, dipBonus, dipDifficulty, dipLanguage, dipCoinage, dipSelfTest]
    .forEach((el) => el.addEventListener("change", saveDips));
  loadDips();

  async function applySet(set, label) {
    if (!set || !set.program || !set.gfx) throw new Error("incomplete ROM set");
    machine.loadRomSet(set.program, set.gfx);
    usingUserRom = set.kind !== "hwtest";
    romKey = crc32(set.program) ^ crc32(set.gfx);
    hiscoreRestored = false;
    // EAROM survives load_roms/reset; clear so restore sees a clean chip.
    writeHiscore(new Array(HISCORE_LEN).fill(0));
    lastSavedHiscore = "";
    syncHiscoreResetBtn();
    setStatus(label);
    await restoreHiscoreNow();
  }

  let hwtestCrcs = null;
  try {
    const c = await (await fetch("roms/crc.json")).json();
    hwtestCrcs = { program: c.program, gfx: c.gfx };
  } catch {}

  function loadBuiltInRom() {
    machine.loadHwtest();
    usingUserRom = false;
    hiscoreRestored = false;
    writeHiscore(new Array(HISCORE_LEN).fill(0));
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
      encodeDsw1,
      encodeDsw2,
      get in0Switches() { return selfTest ? 0x00 : 0x20; },
      get trackPos() { return { x: track.x, y: track.y }; },
      readHiscore,
      writeHiscore,
      saveHiscoreNow,
      maybeSaveHiscore,
      resetHiscoreNow,
      restoreHiscoreNow,
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
