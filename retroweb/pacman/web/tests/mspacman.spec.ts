import { test, expect } from "@playwright/test";
import { existsSync, readFileSync } from "fs";
import path from "path";

// Ms. Pac-Man is the same /pacman/ page with ?game=mspacman: GCC aux board
// in the Z80 socket, not a second machine. Self-test ROM still boots until
// the visitor loads U5/U6/U7. See PACMAN_REVIEW.md §1.

const HWTEST_FILES = [
  "roms/program.bin",
  "roms/pacman.5e",
  "roms/pacman.5f",
  "roms/82s123.7f",
  "roms/82s126.4a",
  "roms/82s126.1m",
];

function buf(path: string, name: string) {
  return { name, mimeType: "application/octet-stream", buffer: readFileSync(path) };
}

function hwtestWithAux() {
  return [
    ...HWTEST_FILES.map((p) => buf(p, p.split("/").pop()!)),
    { name: "u5", mimeType: "application/octet-stream", buffer: Buffer.alloc(0x800, 0) },
    { name: "u6", mimeType: "application/octet-stream", buffer: Buffer.alloc(0x1000, 0) },
    { name: "u7", mimeType: "application/octet-stream", buffer: Buffer.alloc(0x1000, 0) },
  ];
}

function localMsPacmanZip() {
  const env = process.env.MSPACMAN_ROM;
  if (env && existsSync(env)) return env;
  const p = path.resolve(__dirname, "../../roms/user/mspacman.zip");
  return existsSync(p) ? p : undefined;
}

test.describe("Ms. Pac-Man aux board page", () => {
  test("?game=mspacman swaps chrome, hides ghost names, keeps the self-test ROM", async ({ page }) => {
    await page.goto("/?game=mspacman&test=1");
    await page.waitForFunction(() => (window as any).__test);
    await expect(page).toHaveTitle(/Ms\. Pac-Man/i);
    await expect(page.locator("h1")).toHaveText(/Ms\. Pac-Man Arcade/i);
    await expect(page.locator(".tagline")).toContainText(/aux board/i);
    await expect(page.locator("#romLegal")).toContainText(/u5/i);
    await expect(page.locator("#dipGhostsRow")).toBeHidden();
    expect(await page.evaluate(() => (window as any).__test.MSPAC)).toBe(true);
    expect(await page.evaluate(() => (window as any).__test.machine.dsw1())).toBe(0xC9);
    await expect(page.locator("#romStatus")).toHaveText("Running test ROM");
    expect(await page.evaluate(() => (window as any).__test.machine.auxBoard())).toBe(false);
  });

  test("the built-in self-test help screen names Ms. Pac-Man", async ({ page }) => {
    await page.goto("/?game=mspacman&test=1");
    await page.waitForFunction(() => (window as any).__test);
    await page.waitForTimeout(4500);
    const title = await page.evaluate(() => {
      const m = (window as any).__test.machine;
      const vramOff = (col: number, row: number) => {
        const c = col - 2, r = row + 2;
        return (c & 0x20) ? r + ((c & 0x1F) << 5) : c + (r << 5);
      };
      const want = "MS PAC-MAN ARCADE";
      const urow = 4, ucol0 = (28 - want.length) >> 1;
      let s = "";
      for (let i = 0; i < want.length; i++) {
        const offs = vramOff(urow, 27 - (ucol0 + i)) & 0x3ff;
        s += String.fromCharCode(m.memRead(0x4000 + offs));
      }
      return s;
    });
    expect(title).toBe("MS PAC-MAN ARCADE");
  });

  test("stock /pacman/ is unchanged Pac-Man", async ({ page }) => {
    await page.goto("/?test=1");
    await page.waitForFunction(() => (window as any).__test);
    await expect(page).toHaveTitle(/^Pac-Man Arcade$/);
    await expect(page.locator("#dipGhostsRow")).toBeVisible();
    expect(await page.evaluate(() => (window as any).__test.MSPAC)).toBe(false);
  });

  test("a local mspacman.zip is accepted as the Midway set with the aux board", async ({ page }) => {
    const zip = localMsPacmanZip();
    test.skip(!zip, "no local mspacman zip — drop one in roms/user/ or set MSPACMAN_ROM");
    await page.goto("/?game=mspacman&test=1");
    await page.waitForFunction(() => (window as any).__test);
    await page.locator("#romFile").setInputFiles(zip!);
    await expect(page.locator("#romStatus")).toHaveText(/Loaded ROM set|Midway mspacman ROM set/);
    expect(await page.evaluate(() => (window as any).__test.usingUserRom)).toBe(true);
    expect(await page.evaluate(() => (window as any).__test.machine.auxBoard())).toBe(true);
  });

  test("a Pac-Man-only set is rejected on the Ms. Pac-Man page", async ({ page }) => {
    await page.goto("/?game=mspacman&test=1");
    await page.waitForFunction(() => (window as any).__test);
    await page.locator("#romFile").setInputFiles(HWTEST_FILES);
    await expect(page.locator("#romStatus")).toContainText(/U5, U6, and U7/i);
    await expect(page.locator("#romErrorHint")).toBeVisible();
    await expect(page.locator("#romErrorCopy")).toContainText(/u5/i);
    expect(await page.evaluate(() => (window as any).__test.machine.auxBoard())).toBe(false);
  });

  test("hwtest plus U5/U6/U7 installs the aux board", async ({ page }) => {
    await page.goto("/?game=mspacman&test=1");
    await page.waitForFunction(() => (window as any).__test);
    await page.locator("#romFile").setInputFiles(hwtestWithAux());
    await expect(page.locator("#romStatus")).toHaveText("Hardware test ROM");
    expect(await page.evaluate(() => (window as any).__test.machine.auxBoard())).toBe(true);
    expect(await page.evaluate(() => (window as any).__test.machine.auxDecode())).toBe(false);
  });

  test("extra U5/U6/U7 on the Pac-Man page are ignored", async ({ page }) => {
    await page.goto("/?test=1");
    await page.waitForFunction(() => (window as any).__test);
    await page.locator("#romFile").setInputFiles(hwtestWithAux());
    await expect(page.locator("#romStatus")).toHaveText("Hardware test ROM");
    expect(await page.evaluate(() => (window as any).__test.machine.auxBoard())).toBe(false);
  });

  test("IndexedDB sets are per-cabinet", async ({ page }) => {
    await page.goto("/?game=mspacman&test=1");
    await page.waitForFunction(() => (window as any).__test);
    await page.locator("#romFile").setInputFiles(hwtestWithAux());
    await expect(page.locator("#romStatus")).toHaveText("Hardware test ROM");
    expect(await page.evaluate(() => (window as any).__test.machine.auxBoard())).toBe(true);

    await page.goto("/?test=1");
    await page.waitForFunction(() => (window as any).__test);
    expect(await page.evaluate(() => (window as any).__test.machine.auxBoard())).toBe(false);

    await page.goto("/?game=mspacman&test=1");
    await page.waitForFunction(() => (window as any).__test);
    expect(await page.evaluate(() => (window as any).__test.machine.auxBoard())).toBe(true);
  });

  test("DIP persist does not leak ghost-names from Pac-Man", async ({ page }) => {
    await page.goto("/?test=1");
    await page.waitForFunction(() => (window as any).__test);
    await page.locator("#dipGhosts").selectOption("0");
    expect(await page.evaluate(() => (window as any).__test.machine.dsw1())).toBe(0x49);

    await page.goto("/?game=mspacman&test=1");
    await page.waitForFunction(() => (window as any).__test);
    expect(await page.evaluate(() => (window as any).__test.machine.dsw1())).toBe(0xC9);
    await expect(page.locator("#dipGhostsRow")).toBeHidden();
  });

  test("loadMsPacmanSet descrambles U5 and trips the $3FF8 latch", async ({ page }) => {
    await page.goto("/?game=mspacman&test=1");
    await page.waitForFunction(() => (window as any).__test);
    const ok = await page.evaluate(async () => {
      const t = (window as any).__test;
      const z = new Uint8Array(0x4000);
      const tiles = new Uint8Array(0x1000);
      const proms = { c: new Uint8Array(0x20), l: new Uint8Array(0x100), w: new Uint8Array(0x100) };
      const u5 = new Uint8Array(0x800);
      u5[0x10] = 0x01; // descrambles to 0x80 at $8008
      const u6 = new Uint8Array(0x1000);
      const u7 = new Uint8Array(0x1000);
      t.machine.loadMsPacmanSet(z, tiles, tiles, proms.c, proms.l, proms.w, u5, u6, u7);
      const before = t.machine.auxDecode();
      t.machine.memRead(0x3FF8);
      return {
        before,
        after: t.machine.auxDecode(),
        board: t.machine.auxBoard(),
        at8008: t.machine.memRead(0x8008),
      };
    });
    expect(ok.board).toBe(true);
    expect(ok.before).toBe(false);
    expect(ok.after).toBe(true);
    expect(ok.at8008).toBe(0x80);
  });
});
