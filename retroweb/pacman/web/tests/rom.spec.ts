import { test, expect } from "@playwright/test";
import { readFileSync } from "fs";

// ROM-set loader: members are mapped by usual MAME/board names and accepted
// when each chip is the original size. CRC32 only labels the generated
// hardware self-test ROM vs. a user dump — it is not a whitelist. Fixtures
// are that generated set (roms/*, built by `make roms`), never Namco Pac-Man.
// See PACMAN_REVIEW.md §8.

const HWTEST_FILES = [
  "roms/program.bin",
  "roms/pacman.5e",
  "roms/pacman.5f",
  "roms/82s123.7f",
  "roms/82s126.4a",
  "roms/82s126.1m",
];

function buf(path: string, name: string, patch?: (b: Buffer) => Buffer) {
  let buffer = readFileSync(path);
  if (patch) buffer = patch(buffer);
  return { name, mimeType: "application/octet-stream", buffer };
}

function hwtestNamed(names: Record<string, string>, patchProgram?: (b: Buffer) => Buffer) {
  return HWTEST_FILES.map((path) => {
    const base = path.split("/").pop()!;
    const patch = base === "program.bin" ? patchProgram : undefined;
    return buf(path, names[base] || base, patch);
  });
}

test.describe("ROM set loader", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
  });

  test("a rejected upload opens a themed dialog with load instructions", async ({ page }) => {
    await page.locator("#romFile").setInputFiles({
      name: "junk.bin",
      mimeType: "application/octet-stream",
      buffer: Buffer.from("this is not a Pac-Man ROM set"),
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

  test("an incomplete set (missing members) is rejected", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(["roms/program.bin", "roms/pacman.5e"]);
    await expect(page.locator("#romStatus")).toContainText("ROM rejected");
  });

  test("tells the visitor which sets are allowed", async ({ page }) => {
    const legal = page.locator(".legal");
    await expect(legal).toContainText(/complete original-board set/i);
    await expect(legal).toContainText(/puckman/);
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

  test("clone-style filenames (puckman.*) with the right sizes are accepted", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(hwtestNamed({
      "pacman.5e": "puckman.5e",
      "pacman.5f": "puckman.5f",
    }));
    await expect(page.locator("#romStatus")).toHaveText("Hardware test ROM");
  });

  test("a too-short chip is rejected", async ({ page }) => {
    const files = hwtestNamed({});
    const tiles = files.find((f) => f.name === "pacman.5e")!;
    tiles.buffer = tiles.buffer.subarray(0, 100);
    await page.locator("#romFile").setInputFiles(files);
    await expect(page.locator("#romStatus")).toContainText("ROM rejected");
  });

  test("an overdump is clipped to the original chip size and accepted", async ({ page }) => {
    const files = hwtestNamed({}).map((f) => {
      if (f.name !== "pacman.5e") return f;
      return { ...f, buffer: Buffer.concat([f.buffer, Buffer.alloc(4096, 0xff)]) };
    });
    await page.locator("#romFile").setInputFiles(files);
    await expect(page.locator("#romStatus")).toHaveText("Hardware test ROM");
  });
});
