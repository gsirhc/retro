"use strict";
// Owns C:'s IndexedDB storage so no read, write, or conversion runs on the
// page's main thread next to the emulator. Jobs run one at a time in
// arrival order, so a load always sees every save queued before it.
//
// C: is stored uncompressed as 64KB chunks keyed "c2:<index>". A chunk that
// is all zero has no record at all, so unwritten space costs nothing.
importScripts("hdd-convert.js");

const DB_NAME = "pc486-hdd", STORE = "hdd";
const META_KEY = "c2-meta";
const CHUNK = 65536;
const MODEL = "AC2250";
const chunkKey = (i) => "c2:" + String(i).padStart(5, "0");
const chunkIndex = (key) => parseInt(key.slice(3), 10);
const CHUNK_RANGE = IDBKeyRange.bound("c2:", "c2:￿");

// Keys written by the builds that stored a 504MB C:.
const LEGACY_KEY = "c-drive";
const LEGACY_CHUNK_META = "hdd-meta";
const LEGACY_FACTORY_KEY = "factory-base";
const LEGACY_FACTORY_META = "factory-base-meta";

let dbPromise = null;
function openDb() {
  if (!dbPromise) {
    dbPromise = new Promise((resolve, reject) => {
      const req = indexedDB.open(DB_NAME, 1);
      req.onupgradeneeded = () => req.result.createObjectStore(STORE);
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error);
    });
  }
  return dbPromise;
}
function reqP(req) {
  return new Promise((resolve, reject) => {
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}
function txDone(tx) {
  return new Promise((resolve, reject) => {
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
    tx.onabort = () => reject(tx.error || new Error("transaction aborted"));
  });
}
async function get(key) {
  const db = await openDb();
  return reqP(db.transaction(STORE, "readonly").objectStore(STORE).get(key));
}
function isZero(u8) {
  const w = new Uint32Array(u8.buffer, u8.byteOffset, u8.byteLength >> 2);
  for (let i = 0; i < w.length; i++) if (w[i]) return false;
  return true;
}

// ---- legacy (504MB) records ----
async function readLegacyFactoryBase() {
  const meta = await get(LEGACY_FACTORY_META);
  if (meta && meta.v === 1 && meta.chunks > 0) {
    const out = new Uint8Array(meta.length);
    for (let i = 0; i < meta.chunks; i++) {
      const chunk = await get(LEGACY_FACTORY_KEY + ":" + i);
      if (!chunk) return null;
      out.set(new Uint8Array(chunk.buffer || chunk), i * meta.chunkSize);
    }
    return out;
  }
  const raw = await get(LEGACY_FACTORY_KEY);
  if (raw instanceof Blob) return new Uint8Array(await raw.arrayBuffer());
  return raw ? new Uint8Array(raw.buffer || raw) : null;
}
async function readLegacy() {
  const rec = await get(LEGACY_KEY);
  if (rec instanceof Blob) {
    const ds = new DecompressionStream("gzip");
    return new Uint8Array(await new Response(rec.stream().pipeThrough(ds)).arrayBuffer());
  }
  if (rec && rec.v === 1 && rec.base === "factory" && Array.isArray(rec.patches)) {
    const base = await readLegacyFactoryBase();
    if (!base) return null;
    for (const p of rec.patches) base.set(new Uint8Array(p.bytes.buffer || p.bytes), p.offset | 0);
    return base;
  }
  // Raw bytes: Chromium refuses a 504MB value, so only another browser has these.
  if (rec) return rec instanceof Uint8Array ? rec : new Uint8Array(rec);
  const meta = await get(LEGACY_CHUNK_META);
  if (meta && meta.v === 1 && meta.chunks > 0) {
    const out = new Uint8Array(meta.length);
    for (let i = 0; i < meta.chunks; i++) {
      const chunk = await get(LEGACY_KEY + ":" + i);
      if (!chunk) return null;
      out.set(new Uint8Array(chunk.buffer || chunk), i * meta.chunkSize);
    }
    return out;
  }
  return null;
}
async function hasLegacy() {
  return !!((await get(LEGACY_KEY)) || (await get(LEGACY_CHUNK_META)));
}
async function deleteLegacy(store) {
  const meta = await reqP(store.get(LEGACY_CHUNK_META));
  if (meta && meta.chunks) for (let i = 0; i < meta.chunks; i++) store.delete(LEGACY_KEY + ":" + i);
  const fmeta = await reqP(store.get(LEGACY_FACTORY_META));
  if (fmeta && fmeta.chunks) for (let i = 0; i < fmeta.chunks; i++) store.delete(LEGACY_FACTORY_KEY + ":" + i);
  for (const k of [LEGACY_KEY, LEGACY_CHUNK_META, LEGACY_FACTORY_KEY, LEGACY_FACTORY_META]) store.delete(k);
}

// ---- current (256MB) records ----
async function writeImage(u8, { factoryFp = null, modified = true, dropLegacy = false } = {}) {
  const db = await openDb();
  const tx = db.transaction(STORE, "readwrite");
  const store = tx.objectStore(STORE);
  store.delete(CHUNK_RANGE);
  for (let i = 0, n = Math.ceil(u8.length / CHUNK); i < n; i++) {
    const piece = u8.subarray(i * CHUNK, Math.min((i + 1) * CHUNK, u8.length));
    if (!isZero(piece)) store.put(piece.slice().buffer, chunkKey(i));
  }
  store.put({ v: 2, length: u8.length, chunkSize: CHUNK, model: MODEL, factoryFp, modified }, META_KEY);
  if (dropLegacy) await deleteLegacy(store);
  await txDone(tx);
}
async function readImage(meta) {
  const db = await openDb();
  const store = db.transaction(STORE, "readonly").objectStore(STORE);
  const [keys, values] = await Promise.all([reqP(store.getAllKeys(CHUNK_RANGE)), reqP(store.getAll(CHUNK_RANGE))]);
  const out = new Uint8Array(meta.length);
  for (let i = 0; i < keys.length; i++) out.set(new Uint8Array(values[i]), chunkIndex(keys[i]) * CHUNK);
  return out;
}

const ops = {
  // What's stored, without reading the image itself.
  async info() {
    const meta = await get(META_KEY);
    const legacy = await hasLegacy();
    if (meta && meta.v === 2) return { result: { kind: "saved", modified: !!meta.modified, factoryFp: meta.factoryFp, legacy } };
    return { result: { kind: legacy ? "legacy" : "none", legacy } };
  },

  // The stored C:, converting a 504MB save on the way when that's all
  // there is. A save that can't be converted stays where it is.
  async load() {
    const meta = await get(META_KEY);
    if (meta && meta.v === 2) {
      const img = await readImage(meta);
      return { result: { status: "saved", modified: !!meta.modified, buffer: img.buffer }, transfer: [img.buffer] };
    }
    let legacy;
    try {
      legacy = await readLegacy();
    } catch (err) {
      return { result: { status: "legacy-unconverted", code: "format", reason: String(err && err.message || err) } };
    }
    if (!legacy) return { result: { status: "none" } };
    let img;
    try {
      img = convertLegacyHdd(legacy, (fraction) => self.postMessage({ progress: { phase: "convert", fraction } }));
    } catch (err) {
      return { result: { status: "legacy-unconverted", code: err.code || "format", reason: err.message } };
    }
    await writeImage(img, { modified: true, dropLegacy: true });
    return { result: { status: "converted", modified: true, buffer: img.buffer }, transfer: [img.buffer] };
  },

  // An uploaded 504MB file, converted and stored.
  async convert({ buffer }) {
    let img;
    try {
      img = convertLegacyHdd(new Uint8Array(buffer), (fraction) => self.postMessage({ progress: { phase: "convert", fraction } }));
    } catch (err) {
      return { result: { ok: false, code: err.code || "format", reason: err.message } };
    }
    await writeImage(img, { modified: true, dropLegacy: true });
    return { result: { ok: true } };
  },

  async replace({ buffer, factoryFp, modified }) {
    await writeImage(new Uint8Array(buffer), { factoryFp, modified, dropLegacy: modified });
    return { result: true };
  },

  async blank({ length }) {
    const db = await openDb();
    const tx = db.transaction(STORE, "readwrite");
    const store = tx.objectStore(STORE);
    store.delete(CHUNK_RANGE);
    store.put({ v: 2, length, chunkSize: CHUNK, model: MODEL, factoryFp: null, modified: true }, META_KEY);
    await deleteLegacy(store);
    await txDone(tx);
    return { result: true };
  },

  // Sector writes since the last save, as {offset, bytes}. Each touched
  // chunk is read, patched and written back in one transaction.
  async patch({ patches }) {
    const db = await openDb();
    const tx = db.transaction(STORE, "readwrite");
    const store = tx.objectStore(STORE);
    const meta = await reqP(store.get(META_KEY));
    if (!meta || meta.v !== 2) {
      tx.abort();
      return { result: { needFull: true } };
    }
    const touched = new Map();
    for (const p of patches) {
      const bytes = new Uint8Array(p.bytes.buffer || p.bytes, p.bytes.byteOffset || 0, p.bytes.byteLength);
      let off = p.offset, pos = 0;
      while (pos < bytes.length) {
        const i = Math.floor(off / CHUNK), within = off - i * CHUNK;
        const len = Math.min(bytes.length - pos, CHUNK - within);
        if (!touched.has(i)) touched.set(i, []);
        touched.get(i).push({ within, bytes: bytes.subarray(pos, pos + len) });
        off += len;
        pos += len;
      }
    }
    const chunks = await Promise.all([...touched.keys()].map((i) => reqP(store.get(chunkKey(i)))));
    [...touched.entries()].forEach(([i, pieces], k) => {
      const size = Math.min(CHUNK, meta.length - i * CHUNK);
      const chunk = chunks[k] ? new Uint8Array(chunks[k]) : new Uint8Array(size);
      for (const { within, bytes } of pieces) chunk.set(bytes, within);
      if (isZero(chunk)) store.delete(chunkKey(i));
      else store.put(chunk.buffer, chunkKey(i));
    });
    if (!meta.modified) store.put({ ...meta, modified: true }, META_KEY);
    await txDone(tx);
    return { result: { needFull: false } };
  },

  // An untouched factory C: from an older factory image.
  async forget() {
    const db = await openDb();
    const tx = db.transaction(STORE, "readwrite");
    const store = tx.objectStore(STORE);
    store.delete(CHUNK_RANGE);
    store.delete(META_KEY);
    await txDone(tx);
    return { result: true };
  },

  // Reset to factory: forget C: and any old 504MB save.
  async clear() {
    const db = await openDb();
    const tx = db.transaction(STORE, "readwrite");
    const store = tx.objectStore(STORE);
    store.delete(CHUNK_RANGE);
    store.delete(META_KEY);
    await deleteLegacy(store);
    await txDone(tx);
    return { result: true };
  },

  // The raw 504MB save a failed conversion left behind, for download.
  async loadLegacy() {
    const img = await readLegacy();
    if (!img) return { result: null };
    const buf = img.byteOffset === 0 && img.byteLength === img.buffer.byteLength ? img.buffer : img.slice().buffer;
    return { result: buf, transfer: [buf] };
  },
};

let queue = Promise.resolve();
self.onmessage = (e) => {
  const { id, op, args } = e.data;
  queue = queue.then(async () => {
    try {
      const { result, transfer } = await ops[op](args || {});
      self.postMessage({ id, ok: true, result }, transfer || []);
    } catch (err) {
      self.postMessage({ id, ok: false, error: String(err && err.message || err) });
    }
  });
};
