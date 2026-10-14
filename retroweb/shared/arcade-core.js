"use strict";
// Arcade front-end pieces every cabinet shares: ROM files, IndexedDB, audio, HIGH SCORE, frame loop.

const ARCADE_STORAGE_BLOCKED = "Warning: Your ROM has been loaded but the browser blocked storage (IndexedDB).  It won't reload when you return.";

function arcadeCrc32(bytes) {
  let c = ~0;
  for (let i = 0; i < bytes.length; i++) {
    c ^= bytes[i];
    for (let j = 0; j < 8; j++) c = (c >>> 1) ^ (0xEDB88320 & -(c & 1));
  }
  return (~c) >>> 0;
}

function arcadeHexCrc(bytes) { return arcadeCrc32(bytes).toString(16).padStart(8, "0"); }

async function arcadeUnzip(buf) {
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

// Lowercased name -> bytes, with zips expanded.
async function arcadeReadRomFiles(items) {
  const files = {};
  for (const f of items) {
    const name = f.name.toLowerCase();
    // File/Blob.bytes is a method on current Chromium; only a Uint8Array is payload.
    const bytes = f.bytes instanceof Uint8Array ? f.bytes : new Uint8Array(await f.arrayBuffer());
    if (name.endsWith(".zip")) {
      Object.assign(files, await arcadeUnzip(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength)));
    } else {
      files[name] = bytes;
    }
  }
  return files;
}

// One "roms" object store. Writes resolve on commit, so a reload right after can't lose them.
function arcadeStore(dbName) {
  let dbp = null;
  function open() {
    if (!dbp) {
      dbp = new Promise((resolve, reject) => {
        const req = indexedDB.open(dbName, 1);
        req.onupgradeneeded = () => req.result.createObjectStore("roms");
        req.onsuccess = () => {
          const db = req.result;
          db.onversionchange = () => { db.close(); dbp = null; };
          db.onclose = () => { dbp = null; };
          resolve(db);
        };
        req.onerror = () => reject(req.error);
      });
      dbp.catch(() => { dbp = null; });
    }
    return dbp;
  }
  async function run(mode, op) {
    const db = await open();
    return new Promise((resolve, reject) => {
      const tx = db.transaction("roms", mode);
      const req = op(tx.objectStore("roms"));
      tx.oncomplete = () => resolve(req.result);
      tx.onerror = () => reject(tx.error);
      tx.onabort = () => reject(tx.error);
    });
  }
  return {
    get: async (key) => (await run("readonly", (s) => s.get(key))) || null,
    set: async (key, val) => { await run("readwrite", (s) => s.put(val, key)); },
    del: async (key) => { await run("readwrite", (s) => s.delete(key)); },
  };
}

// localStorage JSON that never throws (private mode, blocked storage).
function arcadePrefs(key) {
  return {
    load() {
      try {
        const raw = localStorage.getItem(key);
        return raw ? JSON.parse(raw) : null;
      } catch {
        return null;
      }
    },
    save(obj) {
      try { localStorage.setItem(key, JSON.stringify(obj)); } catch {}
    },
  };
}

// Mute button plus an AudioWorklet fed with machine.drainAudio() each frame.
function arcadeAudio(machine, muteBtn, processorName) {
  const isMuted = () => muteBtn.getAttribute("aria-pressed") === "true";
  muteBtn.addEventListener("click", () => {
    const next = !isMuted();
    muteBtn.setAttribute("aria-pressed", next ? "true" : "false");
    muteBtn.title = next ? "Unmute" : "Mute";
    muteBtn.setAttribute("aria-label", next ? "Unmute" : "Mute");
  });

  let audioCtx = null, audioNode = null;
  async function ensure() {
    if (isMuted()) return;
    if (audioCtx) {
      if (audioCtx.state === "suspended") await audioCtx.resume();
      return;
    }
    audioCtx = new (window.AudioContext || window.webkitAudioContext)({ latencyHint: "interactive" });
    await audioCtx.resume();
    // Output device rate varies, don't assume 48 kHz.
    machine.setAudioHz(audioCtx.sampleRate);
    machine.drainAudio();
    if (!audioCtx.audioWorklet) return;
    const src = `registerProcessor("${processorName}", class extends AudioWorkletProcessor {
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
    audioNode = new AudioWorkletNode(audioCtx, processorName);
    audioNode.connect(audioCtx.destination);
  }

  function pump() {
    if (!audioNode || isMuted() || audioCtx.state !== "running") {
      machine.drainAudio();
      return;
    }
    const samples = machine.drainAudio();
    if (samples.length) audioNode.port.postMessage(samples, [samples.buffer]);
  }

  document.addEventListener("click", () => { ensure().catch(() => {}); }, { once: true });
  return { ensure, pump, isMuted };
}

// HIGH SCORE table saved per ROM set under "hiscores". ready() gates the restore;
// restore(saved), shouldSave(bytes) and onSaved(bytes) override the plain table copy.
function arcadeHiscore(opts) {
  const { store, len, read, write } = opts;
  const ready = opts.ready || (() => true);
  const shouldSave = opts.shouldSave || ((b) => !opts.isFactory(b));
  const restore = opts.restore || ((saved) => {
    if (saved && saved.length >= len) write(saved);
    return read();
  });
  let restored = false;
  let lastSaved = "";
  let gen = 0;
  let restoreBusy = false;
  let saveBusy = false;

  async function loadAll() {
    const all = await store.get("hiscores");
    if (!all || typeof all !== "object" || Array.isArray(all)) return {};
    return { ...all };
  }

  async function saveNow(bytes) {
    const key = opts.key();
    const g = gen;
    const all = await loadAll();
    all[key] = Array.from(bytes).slice(0, len).map((x) => x & 0xff);
    await store.set("hiscores", all);
    if (g !== gen) return;
    lastSaved = bytes.join(",");
    if (opts.onSaved) opts.onSaved(bytes);
  }

  async function restoreNow() {
    if (!opts.active() || restoreBusy) return false;
    restoreBusy = true;
    const g = gen;
    try {
      const all = await loadAll();
      if (g !== gen || !opts.active()) return false;
      const live = restore(all[opts.key()] || null);
      restored = true;
      lastSaved = (live || read()).join(",");
      return true;
    } finally {
      restoreBusy = false;
    }
  }

  async function maybeRestore() {
    if (restored || restoreBusy || !opts.active() || !ready()) return;
    await restoreNow();
  }

  async function maybeSave() {
    if (!restored || saveBusy || !opts.active()) return;
    const cur = read();
    if (!shouldSave(cur)) return;
    if (cur.join(",") === lastSaved) return;
    saveBusy = true;
    try { await saveNow(cur); }
    finally { saveBusy = false; }
  }

  // New ROM set: wait for its own gate before restoring.
  function rearm() {
    gen++;
    restored = false;
    lastSaved = "";
  }

  async function forget() {
    gen++;
    restored = false;
    lastSaved = read().join(",");
    const key = opts.key();
    const all = await loadAll();
    delete all[key];
    await store.set("hiscores", all);
  }

  return {
    get restored() { return restored; },
    ready,
    loadAll,
    saveNow,
    restoreNow,
    maybeRestore,
    maybeSave,
    rearm,
    forget,
  };
}

// #resetHiscore and its confirm dialog. Returns sync(), which enables the button for a user ROM.
function arcadeHiscoreReset(active, confirm) {
  const btn = document.getElementById("resetHiscore");
  const hint = document.getElementById("hiscoreResetHint");
  btn.addEventListener("click", () => {
    if (active() && hint && !hint.open) hint.showModal();
  });
  document.getElementById("hiscoreResetCancel")?.addEventListener("click", () => hint?.close());
  document.getElementById("hiscoreResetOk")?.addEventListener("click", async () => {
    hint?.close();
    await confirm();
  });
  return () => { btn.disabled = !active(); };
}

// Load ROMs / Remove ROMs, the rejection dialog, and the stored set.
function arcadeRomLoader(opts) {
  const { store, key, setStatus } = opts;
  const hint = document.getElementById("romErrorHint");
  document.getElementById("romErrorOk")?.addEventListener("click", () => hint?.close());
  const showError = () => { if (hint && !hint.open) hint.showModal(); };
  const hideError = () => { if (hint && hint.open) hint.close(); };

  async function ingest(items) {
    const set = opts.identify(await arcadeReadRomFiles(items));
    if (!set) throw new Error("incomplete or wrong-sized ROM set");
    await opts.apply(set, false);
    try { await store.set(key, set); } catch { setStatus(ARCADE_STORAGE_BLOCKED); }
    hideError();
  }

  document.getElementById("loadRomBtn").addEventListener("click", () => {
    document.getElementById("romFile").click();
  });
  document.getElementById("romFile").addEventListener("change", async (ev) => {
    const list = [...ev.target.files];
    ev.target.value = "";
    try {
      await ingest(list);
    } catch (e) {
      setStatus("ROM rejected: " + e.message);
      showError();
    }
  });
  document.getElementById("removeRomBtn").addEventListener("click", async () => {
    try { await store.del(key); } catch {}
    opts.builtIn();
  });

  async function boot() {
    let stored = null;
    try { stored = await store.get(key); } catch {}
    if (!stored) return;
    try {
      await opts.apply(stored, true);
    } catch {
      opts.builtIn();
      setStatus("Stored ROM unreadable. Using test ROM");
      showError();
    }
  }

  return { boot, ingest, showError, hideError };
}

// Runs the guest at hz against wall-clock time. A frame longer than 80 ms
// (background tab, stall) is clamped, never caught up.
function arcadeLoop(opts) {
  let lastT = null;
  function tick(t) {
    if (lastT === null) lastT = t;
    let dt = (t - lastT) / 1000;
    lastT = t;
    if (dt > 0.08) dt = 0.08;
    if (opts.beforeRun) opts.beforeRun(dt);
    const cycles = Math.floor(opts.hz * dt);
    if (cycles > 0) opts.machine.runCycles(cycles);
    if (opts.hiscore) {
      opts.hiscore.maybeRestore().catch(() => {});
      opts.hiscore.maybeSave().catch(() => {});
    }
    if (opts.afterRun) opts.afterRun();
    opts.blit();
    opts.audio.pump();
    requestAnimationFrame(tick);
  }
  requestAnimationFrame(tick);
}
