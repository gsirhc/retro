// Pac-Man arcade front end. Guest CPU is always 3.072 MHz vs wall clock.

const CPU_HZ = 3_072_000;
const IDB_NAME = "retroweb-pacman";
const IDB_STORE = "roms";
const TEST = new URLSearchParams(location.search).has("test");

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

function identifySet(files, hwtestCrcs) {
  const get = (n) => files[n] || files[n.toUpperCase()];
  const parts = {
    tiles: get("pacman.5e"),
    sprites: get("pacman.5f"),
    color: get("82s123.7f"),
    lookup: get("82s126.4a"),
    wave: get("82s126.1m"),
  };
  const progNames = ["pacman.6e", "pacman.6f", "pacman.6h", "pacman.6j"];
  if (progNames.every((n) => get(n)) && Object.values(parts).every(Boolean)) {
    const mame = progNames.every((n) => crc32(get(n)) === MAME_PACMAN[n]) &&
      crc32(parts.tiles) === MAME_PACMAN["pacman.5e"] &&
      crc32(parts.sprites) === MAME_PACMAN["pacman.5f"] &&
      crc32(parts.color) === MAME_PACMAN["82s123.7f"] &&
      crc32(parts.lookup) === MAME_PACMAN["82s126.4a"] &&
      crc32(parts.wave) === MAME_PACMAN["82s126.1m"];
    const test = hwtestCrcs && progNames.every((n) => hexCrc(get(n)) === hwtestCrcs[n]);
    if (!mame && !test) return null;
    return {
      kind: mame ? "mame-pacman" : "hwtest",
      program: concat4(progNames.map(get)),
      ...parts,
    };
  }
  if (get("program.bin") && Object.values(parts).every(Boolean)) {
    if (hwtestCrcs && hexCrc(get("program.bin")) !== hwtestCrcs["program.bin"]) return null;
    return { kind: "hwtest", program: get("program.bin"), ...parts };
  }
  return null;
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

PacmanArcade().then(async (Module) => {
  const machine = new Module.Machine();
  const screen = document.getElementById("screen");
  const ctx = screen.getContext("2d");
  const img = ctx.createImageData(224, 288);
  const status = document.getElementById("romStatus");
  const mute = document.getElementById("mute");

  let usingUserRom = false;
  let in0 = 0xFF, in1 = 0xFF;
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
    if (down("Digit5") || down("Numpad5") || Date.now() < coinUntil) n0 &= ~0x20;
    if (down("Digit1") || down("Numpad1")) n1 &= ~0x20;
    if (down("Digit2") || down("Numpad2")) n1 &= ~0x40;
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

  function applySet(set, label) {
    if (!set || !set.program || !set.tiles || !set.sprites || !set.color || !set.lookup || !set.wave)
      throw new Error("incomplete ROM set");
    machine.loadRomSet(set.program, set.tiles, set.sprites, set.color, set.lookup, set.wave);
    usingUserRom = set.kind !== "hwtest";
    setStatus(label);
  }

  let hwtestCrcs = null;
  try { hwtestCrcs = await (await fetch("roms/crc.json")).json(); } catch {}

  const stored = await idbGet("set");
  if (stored) {
    try { applySet(stored, "Loaded stored ROM set"); }
    catch (e) { setStatus("Stored ROM unreadable — using test ROM"); }
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
      if (!set) throw new Error("not a Midway pacman set or test ROM");
      applySet(set, set.kind === "mame-pacman" ? "Midway pacman ROM set" : "Hardware test ROM");
      await idbSet("set", set);
    } catch (e) {
      setStatus("ROM rejected: " + e.message);
    }
  });

  document.getElementById("removeRomBtn").addEventListener("click", async () => {
    await idbDel("set");
    machine.loadHwtest();
    usingUserRom = false;
    setStatus("Running test ROM");
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
      crc32,
        identifySet,
        hwtestCrcs,
      applySet,
    };
  }
});
