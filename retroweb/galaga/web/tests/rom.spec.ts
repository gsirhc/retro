import { test, expect } from "@playwright/test";
import { readFileSync } from "fs";

const PARTS = [
  "roms/main.bin",
  "roms/sub.bin",
  "roms/sound.bin",
  "roms/2600j.bin",
  "roms/sprites.bin",
  "roms/prom-5.5n",
  "roms/prom-4.2n",
  "roms/prom-3.1c",
  "roms/prom-1.1d",
];

function buf(path: string, name?: string, patch?: (b: Buffer) => Buffer) {
  let buffer = readFileSync(path);
  if (patch) buffer = patch(Buffer.from(buffer));
  return { name: name || path.split("/").pop()!, mimeType: "application/octet-stream", buffer };
}

function hwtestNamed(patchMain?: (b: Buffer) => Buffer) {
  return PARTS.map((path) => {
    const base = path.split("/").pop()!;
    return buf(path, base, base === "main.bin" ? patchMain : undefined);
  });
}

function mcu(name: string, n = 0x400) {
  return { name, mimeType: "application/octet-stream", buffer: Buffer.alloc(n, 0xA5) };
}

function crc32(data: Buffer) {
  let c = ~0;
  for (let i = 0; i < data.length; i++) {
    c ^= data[i];
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
    local.writeUInt16LE(0, 8);
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

function userParts() {
  return hwtestNamed((b) => {
    b[0] ^= 1;
    return b;
  });
}

const HLE51 = "51xx.bin missing — 51XX fell back to HLE";
const HLE54 = "54xx.bin missing — 54XX fell back to HLE";

test.describe("ROM set loader", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await page.waitForFunction(() => (window as any).__test);
  });

  test("a rejected upload opens a themed dialog with load instructions", async ({ page }) => {
    await page.locator("#romFile").setInputFiles({
      name: "junk.bin",
      mimeType: "application/octet-stream",
      buffer: Buffer.from("this is not a Galaga ROM set"),
    });
    const dlg = page.locator("#romErrorHint");
    await expect(dlg).toBeVisible();
    await expect(dlg).toContainText(/complete original-board set/i);
    await expect(dlg).toContainText(/checked by size, not a particular CRC/i);
    await expect(page.locator("#romStatus")).toContainText("ROM rejected");
    expect(await page.evaluate(() => (window as any).__test.usingUserRom)).toBe(false);
    await page.locator("#romErrorOk").click();
    await expect(dlg).toBeHidden();
  });

  test("re-loading the generated hardware self-test ROM stays on the test line", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(PARTS);
    await expect(page.locator("#romStatus")).toHaveText("Running test ROM");
    expect(await page.evaluate(() => (window as any).__test.usingUserRom)).toBe(false);
  });

  test("an accepted set is persisted in IndexedDB and restored on reload", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(PARTS);
    await expect(page.locator("#romStatus")).toHaveText("Running test ROM");
    // The status line does not change for the self-test, so wait until the
    // write has committed before reloading.
    await expect.poll(() => page.evaluate(() => new Promise((resolve) => {
      const req = indexedDB.open("retroweb-galaga", 1);
      req.onsuccess = () => {
        const g = req.result.transaction("roms").objectStore("roms").get("set");
        g.onsuccess = () => resolve(g.result ? g.result.kind : "");
        g.onerror = () => resolve("");
      };
      req.onerror = () => resolve("");
    }))).toBe("hwtest");
    await page.reload();
    await expect(page.locator("#romStatus")).toHaveText("Loaded stored ROM set");
  });

  test("Remove ROMs clears the stored set and reverts to the test ROM", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(PARTS);
    await expect(page.locator("#romStatus")).toHaveText("Running test ROM");
    await page.locator("#removeRomBtn").click();
    await expect(page.locator("#romStatus")).toHaveText("Running test ROM");
    await page.reload();
    await expect(page.locator("#romStatus")).toHaveText("Running test ROM");
  });

  test("an incomplete set is rejected", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(["roms/main.bin"]);
    await expect(page.locator("#romStatus")).toContainText("ROM rejected");
  });

  test("a wrong-sized game chip is rejected", async ({ page }) => {
    const files = userParts().map((f) =>
      f.name === "3600e.bin" ? { ...f, name: "3600e.bin", buffer: Buffer.alloc(100) } : f);
    const renamed = files.map((f) => {
      if (f.name === "sub.bin") return { ...f, name: "3600e.bin", buffer: Buffer.alloc(100) };
      if (f.name === "sound.bin") return { ...f, name: "3700g.bin" };
      if (f.name === "main.bin") {
        return [
          { name: "3200a.bin", mimeType: f.mimeType, buffer: f.buffer.subarray(0, 0x1000) },
          { name: "3300b.bin", mimeType: f.mimeType, buffer: f.buffer.subarray(0x1000, 0x2000) },
          { name: "3400c.bin", mimeType: f.mimeType, buffer: f.buffer.subarray(0x2000, 0x3000) },
          { name: "3500d.bin", mimeType: f.mimeType, buffer: f.buffer.subarray(0x3000, 0x4000) },
        ];
      }
      return f;
    }).flat();
    await page.locator("#romFile").setInputFiles(renamed);
    await expect(page.locator("#romStatus")).toContainText("ROM rejected");
    await expect(page.locator("#romErrorHint")).toBeVisible();
  });

  test("tells the visitor which sets are allowed", async ({ page }) => {
    const legal = page.locator("#romLegal");
    await expect(legal).toContainText(/complete original-board set/i);
    await expect(legal).toContainText(/galagamw/);
    await expect(legal).toContainText(/checked by size, not a particular CRC/i);
  });

  test("a user set with both MCU files keeps the loaded-set line", async ({ page }) => {
    const files = [...userParts(), mcu("51xx.bin"), mcu("54xx.bin")];
    const zip = zipStore(files.map((f) => ({ name: f.name, data: f.buffer })));
    await page.locator("#romFile").setInputFiles({
      name: "galagamw.zip",
      mimeType: "application/zip",
      buffer: zip,
    });
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    expect(await page.evaluate(() => (window as any).__test.machine.mcu51Hle())).toBe(false);
    expect(await page.evaluate(() => (window as any).__test.machine.mcu54Hle())).toBe(false);
  });

  test("a zip missing 51xx.bin names only that chip", async ({ page }) => {
    const files = [...userParts(), mcu("54xx.bin")];
    const zip = zipStore(files.map((f) => ({ name: f.name, data: f.buffer })));
    await page.locator("#romFile").setInputFiles({ name: "galagamw.zip", mimeType: "application/zip", buffer: zip });
    await expect(page.locator("#romStatus")).toHaveText(HLE51);
    expect(await page.evaluate(() => (window as any).__test.machine.mcu51Hle())).toBe(true);
    expect(await page.evaluate(() => (window as any).__test.machine.mcu54Hle())).toBe(false);
  });

  test("a zip missing 54xx.bin names only that chip", async ({ page }) => {
    const files = [...userParts(), mcu("51xx.bin")];
    const zip = zipStore(files.map((f) => ({ name: f.name, data: f.buffer })));
    await page.locator("#romFile").setInputFiles({ name: "galagamw.zip", mimeType: "application/zip", buffer: zip });
    await expect(page.locator("#romStatus")).toHaveText(HLE54);
    expect(await page.evaluate(() => (window as any).__test.machine.mcu54Hle())).toBe(true);
    expect(await page.evaluate(() => (window as any).__test.machine.mcu51Hle())).toBe(false);
  });

  test("a zip missing both MCU files names each one", async ({ page }) => {
    const zip = zipStore(userParts().map((f) => ({ name: f.name, data: f.buffer })));
    await page.locator("#romFile").setInputFiles({ name: "galagamw.zip", mimeType: "application/zip", buffer: zip });
    await expect(page.locator("#romStatus")).toHaveText(`${HLE51}. ${HLE54}`);
  });

  test("a wrong-sized 51xx.bin falls back the same way as a missing file", async ({ page }) => {
    const files = [...userParts(), mcu("51xx.bin", 16), mcu("54xx.bin")];
    const zip = zipStore(files.map((f) => ({ name: f.name, data: f.buffer })));
    await page.locator("#romFile").setInputFiles({ name: "galagamw.zip", mimeType: "application/zip", buffer: zip });
    await expect(page.locator("#romStatus")).toHaveText(HLE51);
  });

  test("Midway chip names with the right sizes are accepted", async ({ page }) => {
    const files = hwtestNamed().flatMap((f) => {
      if (f.name === "main.bin") {
        return ["3200a.bin", "3300b.bin", "3400c.bin", "3500d.bin"].map((name, i) => ({
          name, mimeType: f.mimeType, buffer: f.buffer.subarray(i * 0x1000, (i + 1) * 0x1000),
        }));
      }
      if (f.name === "sub.bin") return [{ ...f, name: "3600e.bin" }];
      if (f.name === "sound.bin") return [{ ...f, name: "3700g.bin" }];
      if (f.name === "sprites.bin") {
        return [
          { name: "2800l.bin", mimeType: f.mimeType, buffer: f.buffer.subarray(0, 0x1000) },
          { name: "2700k.bin", mimeType: f.mimeType, buffer: f.buffer.subarray(0x1000, 0x2000) },
        ];
      }
      return [f];
    });
    await page.locator("#romFile").setInputFiles(files);
    await expect(page.locator("#romStatus")).toHaveText("Running test ROM");
  });
});
