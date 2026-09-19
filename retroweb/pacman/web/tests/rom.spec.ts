import { test, expect } from "@playwright/test";

// ROM-set loader: the file picker accepts a MAME-shaped romset (loose files
// or a .zip) and identifies members by public CRC32, never by trusting a
// filename alone. These fixtures are the *generated hardware self-test ROM*
// (roms/*.bin etc., built by `make roms` before this suite runs) -- never
// Namco Pac-Man -- so this suite can exercise "accept a known-good set" and
// "reject an unknown one" without any copyrighted bytes anywhere in the repo.
// See PACMAN_REVIEW.md §8.

const HWTEST_FILES = [
  "roms/program.bin",
  "roms/pacman.5e",
  "roms/pacman.5f",
  "roms/82s123.7f",
  "roms/82s126.4a",
  "roms/82s126.1m",
];

test.describe("ROM set loader", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
  });

  test("rejects a garbage upload and keeps running the test ROM", async ({ page }) => {
    await page.locator("#romFile").setInputFiles({
      name: "junk.bin",
      mimeType: "application/octet-stream",
      buffer: Buffer.from("this is not a Pac-Man ROM set"),
    });
    await expect(page.locator("#romStatus")).toContainText("ROM rejected");
    expect(await page.evaluate(() => (window as any).__test.usingUserRom)).toBe(false);
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
});
