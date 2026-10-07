import { test, expect } from "@playwright/test";
import { readFileSync } from "fs";

// Members map by MAME/board names and must be original chip size. CRC32 only
// labels the self-test ROM vs a user dump.

const HWTEST_FILES = [
  "roms/035145-04e.ef2",
  "roms/035144-04e.h2",
  "roms/035143-02.j2",
  "roms/035127-02.np3",
];

function buf(path: string, name: string, patch?: (b: Buffer) => Buffer) {
  let buffer = readFileSync(path);
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
  });

  test("a rejected upload opens a themed dialog with load instructions", async ({ page }) => {
    await page.locator("#romFile").setInputFiles({
      name: "junk.bin",
      mimeType: "application/octet-stream",
      buffer: Buffer.from("this is not an Asteroids ROM set"),
    });
    const dlg = page.locator("#romErrorHint");
    await expect(dlg).toBeVisible();
    await expect(dlg).toContainText(/complete original-board set/i);
    await expect(dlg).toContainText(/checked by size, not a particular CRC/i);
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

    await page.reload();
    await expect(page.locator("#romStatus")).toHaveText("Running test ROM");
  });

  test("an incomplete set (missing the vector ROM) is rejected", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(["roms/035145-04e.ef2", "roms/035144-04e.h2"]);
    await expect(page.locator("#romStatus")).toContainText("ROM rejected");
  });

  test("tells the visitor which sets are allowed", async ({ page }) => {
    const legal = page.locator(".legal").first();
    await expect(legal).toContainText(/complete original-board set/i);
    await expect(legal).toContainText(/asteroid/i);
    await expect(legal).toContainText(/checked by size, not a particular CRC/i);
  });

  test("a complete set with a different CRC is still accepted as a user ROM", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(hwtestNamed({}, (b) => {
      const out = Buffer.from(b);
      out[0] ^= 1;
      return out;
    }));
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    expect(await page.evaluate(() => (window as any).__test.usingUserRom)).toBe(true);
  });

  test("whole-image program.bin + vector.bin are accepted the same as loose chips", async ({ page }) => {
    const files = [
      buf("roms/program.bin", "program.bin"),
      buf("roms/vector.bin", "vector.bin"),
    ];
    await page.locator("#romFile").setInputFiles(files);
    await expect(page.locator("#romStatus")).toHaveText("Hardware test ROM");
  });

  test("a too-short chip is rejected", async ({ page }) => {
    const files = hwtestNamed({});
    const chip = files.find((f) => f.name === "035127-02.np3")!;
    chip.buffer = Buffer.from(chip.buffer.subarray(0, 100));
    await page.waitForFunction(() => (window as any).__test);
    await page.locator("#romFile").setInputFiles(files);
    await expect(page.locator("#romStatus")).toContainText("ROM rejected");
  });

  test("an overdump is clipped to the original chip size and accepted", async ({ page }) => {
    const files = hwtestNamed({}).map((f) => {
      if (f.name !== "035127-02.np3") return f;
      return { ...f, buffer: Buffer.concat([f.buffer, Buffer.alloc(2048, 0xff)]) };
    });
    await page.locator("#romFile").setInputFiles(files);
    await expect(page.locator("#romStatus")).toHaveText("Hardware test ROM");
  });

  test("a .zip of the generated set is accepted", async ({ page }) => {
    const members = hwtestNamed({});
    const zip = zipStore(members.map((f) => ({ name: f.name, data: f.buffer })));
    await page.locator("#romFile").setInputFiles({
      name: "asteroid.zip",
      mimeType: "application/zip",
      buffer: zip,
    });
    await expect(page.locator("#romStatus")).toHaveText("Hardware test ROM");
  });

  test("the optional 034602 bonus-coin PROM is ignored, not required or rejected", async ({ page }) => {
    const files = hwtestNamed({});
    files.push(buf("roms/035127-02.np3", "034602.r1"));
    await page.locator("#romFile").setInputFiles(files);
    await expect(page.locator("#romStatus")).toHaveText("Hardware test ROM");
  });
});
