import { test, expect } from "@playwright/test";
import { readFileSync } from "fs";

import { HWTEST_FILES } from "./cabinet";

function userSet() {
  return HWTEST_FILES.map((path) => {
    const name = path.split("/").pop()!;
    let buffer = readFileSync(path);
    if (name === "lrescue.1") {
      buffer = Buffer.from(buffer);
      buffer[0] ^= 1;
    }
    return { name, mimeType: "application/octet-stream" as const, buffer };
  });
}

// $20CF x10, $20DB, $20F4-$20F5 (MAME hiscore.dat), and what init leaves there.
const FACTORY = [0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x0A, 0x00, 0x05];
const SAMPLE = [0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x0A, 0x50, 0x12];

// The self-test ROM never runs the game's init, so stand in for it.
const gameInit = (page: import("@playwright/test").Page) =>
  page.evaluate((b) => (window as any).__test.writeHiscore(b), FACTORY);

test.describe("HIGH SCORE persist", () => {
  test.describe.configure({ mode: "serial" });
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await page.waitForFunction(() => (window as any).__test);
  });

  test("Reset is present and disabled on the self-test ROM", async ({ page }) => {
    const btn = page.locator("#resetHiscore");
    await expect(btn).toBeVisible();
    await expect(btn).toBeDisabled();
    await expect(page.locator("p.legal").filter({ hasText: "HIGH SCORE is kept" })).toBeVisible();
  });

  test("HI-SCORE RAM is saved and poked back after a reload", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    await expect(page.locator("#resetHiscore")).toBeEnabled();

    await page.evaluate(async (bytes) => {
      const t = (window as any).__test;
      t.writeHiscore(bytes);
      await t.saveHiscoreNow(t.readHiscore());
    }, SAMPLE);

    await page.reload();
    await expect(page.locator("#romStatus")).toHaveText("Loaded stored ROM set");
    await page.waitForFunction(() => (window as any).__test.machine.state().frames >= 30);
    await gameInit(page);

    await expect.poll(() => page.evaluate(() => (window as any).__test.hiscoreRestored)).toBe(true);
    expect(await page.evaluate(() => (window as any).__test.readHiscore())).toEqual(SAMPLE);
  });

  test("the restore waits until init leaves the factory table", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    await page.evaluate(async (bytes) => {
      const t = (window as any).__test;
      t.writeHiscore(bytes);
      await t.saveHiscoreNow(t.readHiscore());
    }, SAMPLE);

    await page.reload();
    await expect(page.locator("#romStatus")).toHaveText("Loaded stored ROM set");
    await page.waitForFunction(() => (window as any).__test.machine.state().frames > 120);
    expect(await page.evaluate(() => (window as any).__test.hiscoreRestored)).toBe(false);

    await gameInit(page);
    await expect.poll(() => page.evaluate(() => (window as any).__test.hiscoreRestored)).toBe(true);
    expect(await page.evaluate(() => (window as any).__test.readHiscore())).toEqual(SAMPLE);
  });

  test("Remove ROMs disables Reset again", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#resetHiscore")).toBeEnabled();
    await page.locator("#removeRomBtn").click();
    await expect(page.locator("#romStatus")).toHaveText("Running test ROM");
    await page.waitForFunction(() => !!(window as any).__test?.machine);
    await expect(page.locator("#resetHiscore")).toBeDisabled();
  });

  test("Reset Cancel leaves the saved table in place", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    await page.evaluate(async (bytes) => {
      const t = (window as any).__test;
      t.writeHiscore(bytes);
      await t.saveHiscoreNow(t.readHiscore());
    }, SAMPLE);

    await page.locator("#resetHiscore").click();
    await expect(page.locator("#hiscoreResetHint")).toBeVisible();
    await page.locator("#hiscoreResetCancel").click();
    await expect(page.locator("#hiscoreResetHint")).toBeHidden();

    const bytes = await page.evaluate(() => (window as any).__test.readHiscore());
    expect(bytes).toEqual(SAMPLE);
  });

  test("Reset confirm clears the saved table", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    await page.evaluate(async (bytes) => {
      const t = (window as any).__test;
      t.writeHiscore(bytes);
      await t.saveHiscoreNow(t.readHiscore());
    }, SAMPLE);

    expect(Object.keys(await page.evaluate(() => (window as any).__test.savedHiscores()))).toHaveLength(1);

    await page.locator("#resetHiscore").click();
    await page.locator("#hiscoreResetOk").click();
    await expect(page.locator("#hiscoreResetHint")).toBeHidden();
    await expect.poll(async () => Object.keys(await page.evaluate(() => (window as any).__test.savedHiscores()))).toHaveLength(0);

    await page.waitForFunction(() => (window as any).__test.machine.state().frames >= 30);
    await gameInit(page);
    await expect.poll(() => page.evaluate(() => (window as any).__test.hiscoreRestored)).toBe(true);
    expect(await page.evaluate(() => (window as any).__test.readHiscore())).toEqual(FACTORY);
  });
});
