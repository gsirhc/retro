"use strict";
// Converts a C: saved by the old 504MB build (1024/16/63) into an image for the WD Caviar
// AC2250 (1010/9/55, 256MB). Files are copied into the layout FreeDOS FDISK/FORMAT gave the
// factory image; the boot code is kept and the BPB rewritten since boot code reads geometry from it.

const kLegacyHddBytes = 528482304;
const kHddBytes = 255974400;

// Factory partition and FAT16 layout, read back with minfo after `make hdd-image`.
const kTarget = {
  mbrEntry: [0x80, 0x01, 0x01, 0x00, 0x06, 0x08, 0xF7, 0xF2, 0x37, 0x00, 0x00, 0x00, 0xB7, 0xA0, 0x07, 0x00],
  partStartLba: 55,
  partSectors: 499895,
  sectorsPerCluster: 8,
  reservedSectors: 1,
  fats: 2,
  rootEntries: 512,
  fatSectors: 244,
  media: 0xF8,
  sectorsPerTrack: 55,
  heads: 9,
};

class HddConvertError extends Error {
  constructor(code, message) {
    super(message);
    this.code = code;
  }
}

function readFat16Volume(src) {
  const dv = new DataView(src.buffer, src.byteOffset, src.byteLength);
  if (dv.getUint16(510, true) !== 0xAA55) throw new HddConvertError("format", "no partition table");
  let part = null;
  for (let i = 0; i < 4; i++) {
    const e = 446 + i * 16;
    const type = src[e + 4];
    if (type === 0) continue;
    if (type !== 0x04 && type !== 0x06 && type !== 0x0E) {
      throw new HddConvertError("format", "partition type 0x" + type.toString(16) + " isn't FAT16");
    }
    if (part) throw new HddConvertError("format", "more than one partition");
    part = { lba: dv.getUint32(e + 8, true), sectors: dv.getUint32(e + 12, true) };
  }
  if (!part) throw new HddConvertError("format", "no partition");
  const base = part.lba * 512;
  if (base + 512 > src.length || dv.getUint16(base + 510, true) !== 0xAA55) {
    throw new HddConvertError("format", "no boot sector");
  }
  const bps = dv.getUint16(base + 0x0B, true);
  const spc = src[base + 0x0D];
  const reserved = dv.getUint16(base + 0x0E, true);
  const fats = src[base + 0x10];
  const rootEntries = dv.getUint16(base + 0x11, true);
  const totalSectors = dv.getUint16(base + 0x13, true) || dv.getUint32(base + 0x20, true);
  const fatSectors = dv.getUint16(base + 0x16, true);
  if (bps !== 512 || !spc || !reserved || !fats || !fatSectors || !rootEntries) {
    throw new HddConvertError("format", "not a FAT16 boot sector");
  }
  const fatOff = base + reserved * 512;
  const rootOff = fatOff + fats * fatSectors * 512;
  const rootBytes = Math.ceil(rootEntries * 32 / 512) * 512;
  const dataOff = rootOff + rootBytes;
  const clusterBytes = spc * 512;
  const clusters = Math.floor((totalSectors - (dataOff - base) / 512) / spc);
  if (clusters < 4085 || clusters >= 65525) throw new HddConvertError("format", "not FAT16");
  if (dataOff + clusters * clusterBytes > src.length) throw new HddConvertError("format", "partition runs past the disk");
  return { src, dv, base, fatOff, rootOff, rootBytes, dataOff, clusterBytes, clusters };
}

function oldChain(vol, start) {
  const out = [];
  let c = start;
  while (c >= 2 && c < vol.clusters + 2) {
    out.push(c);
    if (out.length > vol.clusters) throw new HddConvertError("format", "FAT chain loops");
    c = vol.dv.getUint16(vol.fatOff + c * 2, true);
  }
  return out;
}

function oldClusterView(vol, c, len) {
  const off = vol.dataOff + (c - 2) * vol.clusterBytes;
  return vol.src.subarray(off, off + len);
}

// Live entries up to the first end-of-directory marker, in 32-byte units.
function usedDirBytes(bytes) {
  for (let o = 0; o < bytes.length; o += 32) if (bytes[o] === 0) return o;
  return bytes.length;
}

function isDotEntry(bytes, o) {
  return bytes[o] === 0x2E && (bytes[o + 1] === 0x20 || (bytes[o + 1] === 0x2E && bytes[o + 2] === 0x20));
}

function readDirTree(vol, bytes, seen) {
  const node = { bytes, children: [], files: [] };
  for (let o = 0; o < bytes.length; o += 32) {
    const first = bytes[o];
    if (first === 0) break;
    const attr = bytes[o + 11];
    if (attr === 0x0F || (attr & 0x08)) continue;
    if (first === 0xE5) {
      bytes[o + 26] = 0;
      bytes[o + 27] = 0;
      continue;
    }
    if (isDotEntry(bytes, o)) continue;
    const start = bytes[o + 26] | (bytes[o + 27] << 8);
    if (attr & 0x10) {
      if (start < 2 || seen.has(start)) throw new HddConvertError("format", "damaged directory");
      seen.add(start);
      const chain = oldChain(vol, start);
      const raw = new Uint8Array(chain.length * vol.clusterBytes);
      chain.forEach((c, i) => raw.set(oldClusterView(vol, c, vol.clusterBytes), i * vol.clusterBytes));
      const child = readDirTree(vol, raw.slice(0, usedDirBytes(raw)), seen);
      child.entryOffset = o;
      node.children.push(child);
    } else {
      const size = (bytes[o + 28] | (bytes[o + 29] << 8) | (bytes[o + 30] << 16) | (bytes[o + 31] << 24)) >>> 0;
      node.files.push({ entryOffset: o, start, size });
    }
  }
  return node;
}

function convertLegacyHdd(src, onProgress) {
  if (src.length !== kLegacyHddBytes) {
    throw new HddConvertError("format", "image is " + src.length + " bytes, not " + kLegacyHddBytes);
  }
  const vol = readFat16Volume(src);
  const t = kTarget;
  const clusterBytes = t.sectorsPerCluster * 512;
  const rootBytes = t.rootEntries * 32;
  const fatOff = (t.partStartLba + t.reservedSectors) * 512;
  const rootOff = fatOff + t.fats * t.fatSectors * 512;
  const dataOff = rootOff + Math.ceil(rootBytes / 512) * 512;
  const clusters = Math.floor((t.partStartLba + t.partSectors - dataOff / 512) / t.sectorsPerCluster);

  const rootRaw = src.slice(vol.rootOff, vol.rootOff + vol.rootBytes);
  const root = readDirTree(vol, rootRaw.subarray(0, usedDirBytes(rootRaw)), new Set());
  if (root.bytes.length > rootBytes) throw new HddConvertError("format", "root directory has too many entries");

  let needed = 0, totalFileBytes = 0;
  (function count(node, isRoot) {
    if (!isRoot) needed += Math.max(1, Math.ceil(node.bytes.length / clusterBytes));
    for (const f of node.files) {
      needed += Math.ceil(f.size / clusterBytes);
      totalFileBytes += f.size;
    }
    for (const c of node.children) count(c, false);
  })(root, true);
  if (needed > clusters) {
    throw new HddConvertError("full", "needs " + needed * clusterBytes + " bytes, the drive holds " + clusters * clusterBytes);
  }

  const dst = new Uint8Array(kHddBytes);
  const ddv = new DataView(dst.buffer);
  const fat = new Uint16Array(clusters + 2);
  fat[0] = 0xFF00 | t.media;
  fat[1] = 0xFFFF;
  let next = 2;
  function alloc(n) {
    const start = next;
    for (let i = 0; i < n; i++) fat[start + i] = i === n - 1 ? 0xFFFF : start + i + 1;
    next += n;
    return start;
  }
  const setStart = (bytes, o, c) => {
    bytes[o + 26] = c & 0xFF;
    bytes[o + 27] = c >> 8;
    bytes[o + 20] = 0;
    bytes[o + 21] = 0;
  };

  let copied = 0, lastReport = 0;
  function copyFile(f) {
    const n = Math.ceil(f.size / clusterBytes);
    if (n === 0) return 0;
    const start = alloc(n);
    const chain = oldChain(vol, f.start);
    let left = f.size, pos = dataOff + (start - 2) * clusterBytes;
    for (const c of chain) {
      if (left <= 0) break;
      const len = Math.min(left, vol.clusterBytes);
      dst.set(oldClusterView(vol, c, len), pos);
      pos += len;
      left -= len;
    }
    copied += f.size;
    if (onProgress && copied - lastReport >= 1 << 20) {
      lastReport = copied;
      onProgress(totalFileBytes ? copied / totalFileBytes : 1);
    }
    return start;
  }

  // Breadth-first, files before subdirectories, so kernel files land in the first clusters.
  const queue = [{ node: root, start: 0, parent: 0 }];
  while (queue.length) {
    const { node, start, parent } = queue.shift();
    const bytes = node.bytes;
    for (const f of node.files) setStart(bytes, f.entryOffset, copyFile(f));
    for (const child of node.children) {
      const n = Math.max(1, Math.ceil(child.bytes.length / clusterBytes));
      const childStart = alloc(n);
      setStart(bytes, child.entryOffset, childStart);
      queue.push({ node: child, start: childStart, parent: start });
    }
    if (start === 0) {
      dst.set(bytes, rootOff);
    } else {
      for (let o = 0; o < bytes.length; o += 32) {
        if (bytes[o] !== 0x2E || !isDotEntry(bytes, o)) continue;
        setStart(bytes, o, bytes[o + 1] === 0x2E ? parent : start);
      }
      dst.set(bytes, dataOff + (start - 2) * clusterBytes);
    }
  }

  for (let i = 0; i < t.fats; i++) {
    const off = fatOff + i * t.fatSectors * 512;
    for (let c = 0; c < fat.length; c++) ddv.setUint16(off + c * 2, fat[c], true);
  }

  const bs = t.partStartLba * 512;
  dst.set(src.subarray(vol.base, vol.base + 512), bs);
  ddv.setUint16(bs + 0x0B, 512, true);
  dst[bs + 0x0D] = t.sectorsPerCluster;
  ddv.setUint16(bs + 0x0E, t.reservedSectors, true);
  dst[bs + 0x10] = t.fats;
  ddv.setUint16(bs + 0x11, t.rootEntries, true);
  ddv.setUint16(bs + 0x13, t.partSectors < 0x10000 ? t.partSectors : 0, true);
  dst[bs + 0x15] = t.media;
  ddv.setUint16(bs + 0x16, t.fatSectors, true);
  ddv.setUint16(bs + 0x18, t.sectorsPerTrack, true);
  ddv.setUint16(bs + 0x1A, t.heads, true);
  ddv.setUint32(bs + 0x1C, t.partStartLba, true);
  ddv.setUint32(bs + 0x20, t.partSectors < 0x10000 ? 0 : t.partSectors, true);

  dst.set(src.subarray(0, 446), 0);
  dst.set(t.mbrEntry, 446);
  ddv.setUint16(510, 0xAA55, true);

  if (onProgress) onProgress(1);
  return dst;
}
