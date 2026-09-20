import { test, expect } from "@playwright/test";
import { readFileSync } from "fs";

// Keep HIGH SCORE is an opt-in labelled departure: the real PCB has no
// battery, so a refresh is a power cycle. See PACMAN_REVIEW.md §9.
// Fixtures are the generated self-test ROM, never Namco Pac-Man.

const HWTEST_FILES = [
  "roms/program.bin",
  "roms/pacman.5e",
  "roms/pacman.5f",
  "roms/82s123.7f",
  "roms/82s126.4a",
  "roms/82s126.1m",
];

function userSet() {
  return HWTEST_FILES.map((path) => {
    const name = path.split("/").pop()!;
    let buffer = readFileSync(path);
    if (name === "program.bin") {
      buffer = Buffer.from(buffer);
      buffer[0] ^= 1;
    }
    return { name, mimeType: "application/octet-stream" as const, buffer };
  });
}

test.describe("Keep HIGH SCORE", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
  });

  test("is off by default — a real board has no battery", async ({ page }) => {
    const box = page.locator("#keepHiscore");
    await expect(box).toBeVisible();
    await expect(box).not.toBeChecked();
    await expect(page.getByText(/pull the plug/i)).toBeVisible();
  });

  test("the checkbox itself survives a reload", async ({ page }) => {
    await page.locator("#keepHiscore").check();
    await page.reload();
    await expect(page.locator("#keepHiscore")).toBeChecked();
  });

  test("with the box on, TOP RAM is saved and poked back after a reload", async ({ page }) => {
    await page.locator("#keepHiscore").check();
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");

    await page.evaluate(async () => {
      const t = (window as any).__test;
      t.writeHiscore([0x00, 0x76, 0x00]);
      await t.saveHiscoreNow(t.readHiscore());
    });

    await page.reload();
    await expect(page.locator("#keepHiscore")).toBeChecked();
    await expect(page.locator("#romStatus")).toHaveText("Loaded stored ROM set");

    const bytes = await page.evaluate(async () => {
      await (window as any).__test.restoreHiscoreNow();
      return (window as any).__test.readHiscore();
    });
    expect(bytes).toEqual([0x00, 0x76, 0x00]);
  });

  test("with the box off, a saved TOP is not restored", async ({ page }) => {
    await page.locator("#keepHiscore").check();
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    await page.evaluate(async () => {
      const t = (window as any).__test;
      t.writeHiscore([0x00, 0x50, 0x00]);
      await t.saveHiscoreNow(t.readHiscore());
    });

    await page.evaluate(() => localStorage.setItem("retroweb.pacman.keepHiscore", "0"));
    await page.reload();
    await expect(page.locator("#keepHiscore")).not.toBeChecked();
    await expect(page.locator("#romStatus")).toHaveText("Loaded stored ROM set");

    const bytes = await page.evaluate(async () => {
      const t = (window as any).__test;
      t.writeHiscore([0x99, 0x99, 0x99]);
      await t.restoreHiscoreNow();
      return t.readHiscore();
    });
    expect(bytes).toEqual([0x99, 0x99, 0x99]);
  });
});
