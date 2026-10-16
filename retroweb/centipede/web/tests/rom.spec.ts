import { test, expect } from "@playwright/test";
import { readFileSync } from "fs";

// Members map by MAME/board names and must be original chip size. CRC32 only
// labels the self-test ROM vs a user dump.

const HWTEST_FILES = [
  "roms/136001-307.d1",
  "roms/136001-308.e1",
  "roms/136001-309.fh1",
  "roms/136001-310.j1",
  "roms/136001-211.f7",
  "roms/136001-212.hj7",
];

function buf(path: string, name: string, patch?: (b: Buffer) => Buffer) {
  let buffer: Buffer = readFileSync(path);
  if (patch) buffer = patch(buffer);
  return { name, mimeType: "application/octet-stream", buffer };
}

function hwtestNamed(names: Record<string, string>, patchFirst?: (b: Buffer) => Buffer) {
  return HWTEST_FILES.map((path, i) => {
    const base = path.split("/").pop()!;
    const patch = i === 0 ? patchFirst : undefined;
    return buf(path, names[base] || base, patch);
  });
}

function crc32(buf: Buffer) {
  let c = ~0;
  for (let i = 0; i < buf.length; i++) {
    c ^= buf[i];
    for (let j = 0; j < 8; j++) c = (c >>> 1) ^ (0xEDB88320 & -(c & 1));
  }
  return (~c) >>> 0;
}

function zipStore(files: { name: string; data: Buffer }[]) {
  const locals: Buffer[] = [];
  const centrals: Buffer[] = [];
  let offset = 0;
  for (const f of files) {
    const name = Buffer.from(f.name);
    const crc = crc32(f.data);
    const local = Buffer.alloc(30 + name.length);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4);
    local.writeUInt16LE(0, 8); // STORE
    local.writeUInt32LE(crc, 14);
    local.writeUInt32LE(f.data.length, 18);
    local.writeUInt32LE(f.data.length, 22);
    local.writeUInt16LE(name.length, 26);
    name.copy(local, 30);
    const piece = Buffer.concat([local, f.data]);
    locals.push(piece);
    const central = Buffer.alloc(46 + name.length);
    central.writeUInt32LE(0x02014b50, 0);
    central.writeUInt16LE(20, 4);
    central.writeUInt16LE(20, 6);
    central.writeUInt32LE(crc, 16);
    central.writeUInt32LE(f.data.length, 20);
    central.writeUInt32LE(f.data.length, 24);
    central.writeUInt16LE(name.length, 28);
    central.writeUInt32LE(offset, 42);
    name.copy(central, 46);
    centrals.push(central);
    offset += piece.length;
  }
  const cd = Buffer.concat(centrals);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(files.length, 8);
  end.writeUInt16LE(files.length, 10);
  end.writeUInt32LE(cd.length, 12);
  end.writeUInt32LE(offset, 16);
  return Buffer.concat([...locals, cd, end]);
}

test.describe("ROM set loader", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await page.waitForFunction(() => !!(window as any).__test?.machine);
  });

  test("a rejected upload opens a themed dialog with load instructions", async ({ page }) => {
    await page.locator("#romFile").setInputFiles({
      name: "junk.bin",
      mimeType: "application/octet-stream",
      buffer: Buffer.from("this is not a Centipede ROM set"),
    });
    const dlg = page.locator("#romErrorHint");
    await expect(dlg).toBeVisible();
    await expect(dlg).toContainText(/isn't a complete set/i);
    await expect(dlg).toContainText(/centiped\.zip/);
    await expect(page.locator("#romStatus")).toContainText("ROM rejected");
    expect(await page.evaluate(() => (window as any).__test.usingUserRom)).toBe(false);

    const bg = await dlg.evaluate((el) => getComputedStyle(el).backgroundColor);
    expect(bg).not.toBe("rgba(0, 0, 0, 0)");

    await page.locator("#romErrorOk").click();
    await expect(dlg).toBeHidden();
  });

  test("re-loading the generated hardware self-test ROM is accepted and identified as hwtest, not a user ROM", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(HWTEST_FILES);
    await expect(page.locator("#romStatus")).toHaveText("Hardware test ROM");
    expect(await page.evaluate(() => (window as any).__test.usingUserRom)).toBe(false);
  });

  test("an accepted set is persisted in IndexedDB and restored on reload", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(HWTEST_FILES);
    await expect(page.locator("#romStatus")).toHaveText("Hardware test ROM");

    await page.reload();
    await expect(page.locator("#romStatus")).toHaveText("Loaded stored ROM set");
  });

  test("Remove ROMs clears the stored set and reverts to the test ROM", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(HWTEST_FILES);
    await expect(page.locator("#romStatus")).toHaveText("Hardware test ROM");

    await page.locator("#removeRomBtn").click();
    await expect(page.locator("#romStatus")).toHaveText("Running test ROM");
    await page.waitForFunction(() => !!(window as any).__test?.machine);

    await page.reload();
    await expect(page.locator("#romStatus")).toHaveText("Running test ROM");
    await page.waitForFunction(() => !!(window as any).__test?.machine);
  });

  test("an incomplete set (missing the graphics ROMs) is rejected", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(HWTEST_FILES.slice(0, 4));
    await expect(page.locator("#romStatus")).toContainText("ROM rejected");
  });

  test("tells the visitor which sets are allowed", async ({ page }) => {
    const legal = page.locator(".legal").first();
    await expect(legal).toContainText(/Load the ROMs yourself/i);
    await expect(legal).toContainText(/centiped\.zip/);
    await expect(legal).toContainText(/checked by size, not a particular CRC/i);
  });

  test("a complete set with a different CRC is still accepted as a user ROM", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(hwtestNamed({}, (b) => {
      const out = Buffer.from(b);
      out[0x7f0] ^= 1;
      return out;
    }));
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    expect(await page.evaluate(() => (window as any).__test.usingUserRom)).toBe(true);
  });

  test("whole-image program.bin + gfx.bin are accepted the same as loose chips", async ({ page }) => {
    const files = [
      buf("roms/program.bin", "program.bin"),
      buf("roms/gfx.bin", "gfx.bin"),
    ];
    await page.locator("#romFile").setInputFiles(files);
    await expect(page.locator("#romStatus")).toHaveText("Hardware test ROM");
  });

  test("a too-short chip is rejected", async ({ page }) => {
    const files = hwtestNamed({});
    const chip = files.find((f) => f.name === "136001-212.hj7")!;
    chip.buffer = Buffer.from(chip.buffer.subarray(0, 100));
    await page.waitForFunction(() => (window as any).__test);
    await page.locator("#romFile").setInputFiles(files);
    await expect(page.locator("#romStatus")).toContainText("ROM rejected");
  });

  test("an overdump is clipped to the original chip size and accepted", async ({ page }) => {
    const files = hwtestNamed({}).map((f) => {
      if (f.name !== "136001-212.hj7") return f;
      return { ...f, buffer: Buffer.concat([f.buffer, Buffer.alloc(2048, 0xff)]) };
    });
    await page.locator("#romFile").setInputFiles(files);
    await expect(page.locator("#romStatus")).toHaveText("Hardware test ROM");
  });

  test("a .zip of the generated set is accepted", async ({ page }) => {
    const members = hwtestNamed({});
    const zip = zipStore(members.map((f) => ({ name: f.name, data: f.buffer })));
    await page.locator("#romFile").setInputFiles({
      name: "centiped.zip",
      mimeType: "application/zip",
      buffer: zip,
    });
    await expect(page.locator("#romStatus")).toHaveText("Hardware test ROM");
  });

  test("the colour PROMs 136001-213 / 214 are ignored, not required or rejected", async ({ page }) => {
    const files = hwtestNamed({});
    files.push(buf("roms/136001-212.hj7", "136001-213.p4"));
    files.push(buf("roms/136001-212.hj7", "136001-214.f7"));
    await page.locator("#romFile").setInputFiles(files);
    await expect(page.locator("#romStatus")).toHaveText("Hardware test ROM");
  });

  test("loose chips are matched by board location alone", async ({ page }) => {
    const locs = ["centiped.d1", "centiped.e1", "centiped.fh1", "centiped.j1", "centiped.f7", "centiped.hj7"];
    const files = HWTEST_FILES.map((p, i) => buf(p, locs[i]));
    await page.locator("#romFile").setInputFiles(files);
    await expect(page.locator("#romStatus")).toHaveText("Hardware test ROM");
  });

  test("chips are matched by the 136001 number in any order", async ({ page }) => {
    const files = hwtestNamed({}).reverse();
    await page.locator("#romFile").setInputFiles(files);
    await expect(page.locator("#romStatus")).toHaveText("Hardware test ROM");
  });

  test("a swapped graphics plane loads as a user ROM, not the self-test ROM", async ({ page }) => {
    const names: Record<string, string> = {
      "136001-211.f7": "136001-212.hj7",
      "136001-212.hj7": "136001-211.f7",
    };
    await page.locator("#romFile").setInputFiles(hwtestNamed(names));
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    expect(await page.evaluate(() => (window as any).__test.usingUserRom)).toBe(true);
  });
});

test("an unreadable stored set falls back to the test ROM and says so", async ({ page }) => {
  await page.goto("/?test=1");
  await page.waitForFunction(() => (window as any).__test);
  await page.evaluate(() => new Promise((resolve, reject) => {
    const req = indexedDB.open("retroweb-centipede", 1);
    req.onupgradeneeded = () => req.result.createObjectStore("roms");
    req.onsuccess = () => {
      const tx = req.result.transaction("roms", "readwrite");
      tx.objectStore("roms").put({ kind: "user" }, (window as any).__test.ROM_KEY);
      tx.oncomplete = () => { req.result.close(); resolve(null); };
      tx.onerror = () => reject(tx.error);
    };
    req.onerror = () => reject(req.error);
  }));
  await page.reload();
  await page.waitForFunction(() => (window as any).__test);
  await expect(page.locator("#romStatus")).toHaveText("Stored ROM unreadable. Using test ROM");
  await expect(page.locator("#romErrorHint")).toBeVisible();
  expect(await page.evaluate(() => (window as any).__test.usingUserRom)).toBe(false);
});

test.describe("with IndexedDB blocked", () => {
  test.beforeEach(async ({ page }) => {
    await page.addInitScript(() => {
      IDBFactory.prototype.open = () => { throw new DOMException("blocked", "SecurityError"); };
    });
    await page.goto("/?test=1");
    await page.waitForFunction(() => (window as any).__test);
  });

  test("the test ROM still boots and runs", async ({ page }) => {
    const c0 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    await expect.poll(() => page.evaluate(() => (window as any).__test.machine.totalCycles())).toBeGreaterThan(c0);
    expect(await page.evaluate(() => (window as any).__test.usingUserRom)).toBe(false);
  });

  test("a user set loads for the session with the same warning", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(hwtestNamed({}, (b) => {
      const out = Buffer.from(b);
      out[0x7f0] ^= 1;
      return out;
    }));
    await expect(page.locator("#romStatus")).toHaveText("Warning: Your ROM has been loaded but the browser blocked storage (IndexedDB).  It won't reload when you return.");
    expect(await page.evaluate(() => (window as any).__test.usingUserRom)).toBe(true);
    const c0 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    await expect.poll(() => page.evaluate(() => (window as any).__test.machine.totalCycles())).toBeGreaterThan(c0);
  });

  test("a loaded set runs for the session and Remove ROMs still reverts", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(HWTEST_FILES);
    await expect(page.locator("#romStatus")).toHaveText("Warning: Your ROM has been loaded but the browser blocked storage (IndexedDB).  It won't reload when you return.");
    await page.locator("#removeRomBtn").click();
    await expect(page.locator("#romStatus")).toHaveText("Running test ROM");
    await page.waitForFunction(() => !!(window as any).__test?.machine);
  });
});
