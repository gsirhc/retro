// Frogger arcade front end. Guest main CPU is always 3.072 MHz vs wall clock.

const CPU_HZ = 3_072_000;
const IDB_NAME = "retroweb-frogger";
const IDB_STORE = "roms";
const TEST = new URLSearchParams(location.search).has("test");
const DIP_KEY = "retroweb.frogger.dips";
const ROM_KEY = "set";

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

function clip(bytes, n) {
  if (!bytes || bytes.length < n) return null;
  return bytes.length === n ? bytes : bytes.subarray(0, n);
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

// MAME frogger / board socket names.
function roleOf(name) {
  const n = String(name).toLowerCase().split("/").pop();
  if (n === "program.bin" || n === "prg.bin") return "program";
  if (n === "sound.bin") return "sound";
  if (n === "gfx.bin") return "gfx";
  if (/pr-91|6l/.test(n)) return "prom";
  if (/\.26$|frogger\.26/.test(n)) return "26";
  if (/\.27$|frogger\.27/.test(n)) return "27";
  if (/frsm3|frogger\.28/.test(n)) return "7";
  if (/\.608$/.test(n)) return "608";
  if (/\.609$/.test(n)) return "609";
  if (/\.610$/.test(n)) return "610";
  if (/\.606$/.test(n)) return "606";
  if (/\.607$/.test(n)) return "607";
  return null;
}

function identifySet(files, hwtestCrcs) {
  const found = {};
  for (const [name, data] of Object.entries(files)) {
    const role = roleOf(name);
    if (!role || found[role]) continue;
    found[role] = data;
  }
  let program = clip(found.program, 0x4000);
  if (!program) {
    const a = clip(found["26"], 0x1000);
    const b = clip(found["27"], 0x1000);
    const c = clip(found["7"], 0x2000) || clip(found["7"], 0x1000);
    if (a && b && c) {
      const last = new Uint8Array(0x2000);
      last.set(c);
      program = concat([a, b, last], 0x4000);
    }
  }
  let sound = clip(found.sound, 0x1800);
  if (!sound) {
    const a = clip(found["608"], 0x800);
    const b = clip(found["609"], 0x800);
    const c = clip(found["610"], 0x800);
    if (a && b && c) sound = concat([a, b, c], 0x1800);
  }
  let gfx = clip(found.gfx, 0x1000);
  if (!gfx) {
    const a = clip(found["607"], 0x800);
    const b = clip(found["606"], 0x800);
    if (a && b) gfx = concat([a, b], 0x1000);
  }
  const color = clip(found.prom, 0x20);
  if (!program || !sound || !gfx || !color) return null;

  let kind = "user";
  if (hwtestCrcs &&
      hexCrc(program) === hwtestCrcs["program.bin"] &&
      hexCrc(sound) === hwtestCrcs["sound.bin"] &&
      hexCrc(gfx) === hwtestCrcs["gfx.bin"] &&
      hexCrc(color) === hwtestCrcs["pr-91.6l"]) {
    kind = "hwtest";
  }
  return { kind, program, sound, gfx, color };
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
const updateFocusHint = initFocusHint(document.getElementById("screen"), () => true);
updateFocusHint();

FroggerArcade().then(async (Module) => {
  const machine = new Module.Machine();
  const screen = document.getElementById("screen");
  const ctx = screen.getContext("2d");
  const img = ctx.createImageData(224, 256);
  const status = document.getElementById("romStatus");
  const mute = document.getElementById("mute");

  let usingUserRom = false;
  const keys = {};
  let coinUntil = 0;
  const coinDoor = document.getElementById("coinDoor");
  const dipCoinage = document.getElementById("dipCoinage");
  const dipLives = document.getElementById("dipLives");
  const dipCabinet = document.getElementById("dipCabinet");

  function applyKeys() {
    let n0 = 0xFF;
    let n1 = (0xFC & ~0x03) | (Number(dipLives.value) & 0x03);
    let n2 = (0xF1 & ~0x0E) | (Number(dipCoinage.value) & 0x06) | (Number(dipCabinet.value) & 0x08);
    const down = (k) => keys[k];
    if (down("ArrowRight") || down("KeyD")) n0 &= ~0x10;
    if (down("ArrowLeft") || down("KeyA")) n0 &= ~0x20;
    if (down("Digit5") || down("Numpad5") || Date.now() < coinUntil) n0 &= ~0x80;
    if (down("Digit1") || down("Numpad1")) n1 &= ~0x80;
    if (down("Digit2") || down("Numpad2")) n1 &= ~0x40;
    if (down("ArrowUp") || down("KeyW")) n2 &= ~0x10;
    if (down("ArrowDown") || down("KeyS")) n2 &= ~0x40;
    machine.setIn0(n0);
    machine.setIn1(n1);
    machine.setIn2(n2);
  }

  function insertCoin() {
    coinUntil = Date.now() + 120;
    applyKeys();
    window.setTimeout(applyKeys, 130);
    coinDoor.classList.add("coined");
    window.setTimeout(() => coinDoor.classList.remove("coined"), 180);
    screen.focus();
    ensureAudio().catch(() => {});
  }
  coinDoor.querySelectorAll("[data-coin]").forEach((el) => {
    el.addEventListener("click", insertCoin);
  });

  window.addEventListener("keydown", (e) => {
    keys[e.code] = true;
    applyKeys();
    if (["ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight"].includes(e.code)) e.preventDefault();
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

  function saveDips() {
    try {
      localStorage.setItem(DIP_KEY, JSON.stringify({
        lives: Number(dipLives.value),
        coinage: Number(dipCoinage.value),
        cabinet: Number(dipCabinet.value),
      }));
    } catch {}
    applyKeys();
  }

  function loadDips() {
    try {
      const raw = localStorage.getItem(DIP_KEY);
      if (raw) {
        const s = JSON.parse(raw);
        if (s.lives !== undefined) dipLives.value = String(s.lives);
        if (s.coinage !== undefined) dipCoinage.value = String(s.coinage);
        if (s.cabinet !== undefined) dipCabinet.value = String(s.cabinet);
      }
    } catch {}
    applyKeys();
  }

  [dipCoinage, dipLives, dipCabinet].forEach((el) => el.addEventListener("change", saveDips));
  loadDips();

  function applySet(set, label) {
    if (!set || !set.program || !set.sound || !set.gfx || !set.color)
      throw new Error("incomplete ROM set");
    machine.loadRomSet(set.program, set.sound, set.gfx, set.color);
    usingUserRom = set.kind !== "hwtest";
    setStatus(label);
  }

  let hwtestCrcs = null;
  try { hwtestCrcs = await (await fetch("roms/crc.json")).json(); } catch {}

  function loadBuiltInRom() {
    machine.loadHwtest();
    usingUserRom = false;
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

  document.getElementById("loadRomBtn").addEventListener("click", () => {
    document.getElementById("romFile").click();
  });

  document.getElementById("romFile").addEventListener("change", async (ev) => {
    const list = [...ev.target.files];
    ev.target.value = "";
    try {
      const files = {};
      for (const f of list) {
        const buf = await f.arrayBuffer();
        if (f.name.toLowerCase().endsWith(".zip")) Object.assign(files, await unzip(buf));
        else files[f.name.toLowerCase()] = new Uint8Array(buf);
      }
      const set = identifySet(files, hwtestCrcs);
      if (!set) throw new Error("incomplete or wrong-sized ROM set");
      applySet(set, labelFor(set.kind));
      await idbSet(ROM_KEY, set);
      hideRomError();
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
      crc32,
      identifySet,
      hwtestCrcs,
      applySet,
      ROM_KEY,
    };
  }
});
