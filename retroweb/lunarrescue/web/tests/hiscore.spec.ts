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
    if (name === "lrescue.4") {
      buffer = Buffer.from(buffer);
      buffer.set(IMAGE_NAME, IMAGE_AT + 0xcf);
      buffer.set(IMAGE_SCORE, IMAGE_AT + 0xf4);
    }
    return { name, mimeType: "application/octet-stream" as const, buffer };
  });
}

// Reset copies ROM $1AA3-$1BA2 to $2000. The self-test chip 4 is blank there, so the spec
// patches in the holder name (blank, blank, T A I T O, blank x3) and the 0500 score.
const IMAGE_AT = 0x1aa3 - 0x1800;
const IMAGE_NAME = [0x1B, 0x1B, 0x13, 0x00, 0x08, 0x13, 0x0E, 0x1B, 0x1B, 0x1B];
const IMAGE_SCORE = [0x00, 0x05];
// $20CF x10, $20DB, $20F4-$20F5. The image has length 0, the first score-line draw stores 10.
const COPIED = [...IMAGE_NAME, 0x00, ...IMAGE_SCORE];
const FACTORY = [...IMAGE_NAME, 0x0A, ...IMAGE_SCORE];
const SAMPLE = [0x1B, 0x1B, 0x1B, 0x1B, 0x13, 0x00, 0x08, 0x13, 0x0E, 0x1B, 0x0A, 0x50, 0x12];

// The self-test ROM never runs the game's init, so stand in for it: the image copy, then the draw.
const gameCopy = (page: import("@playwright/test").Page) =>
  page.evaluate((b) => (window as any).__test.writeHiscore(b), COPIED);
const gameInit = async (page: import("@playwright/test").Page) => {
  await gameCopy(page);
  await page.evaluate((b) => (window as any).__test.writeHiscore(b), FACTORY);
};

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

  test("the factory table is the ROM image with the drawn name length", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    expect(await page.evaluate(() => (window as any).__test.factoryHiscore())).toEqual(FACTORY);
  });

  test("the factory table is never saved", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    await gameInit(page);
    await page.waitForFunction(() => (window as any).__test.machine.state().frames >= 30);
    await gameInit(page);
    const saved = await page.evaluate(async () => {
      const t = (window as any).__test;
      await t.maybeSaveHiscore();
      return Object.keys(await t.savedHiscores());
    });
    expect(saved).toEqual([]);
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

    await gameCopy(page);
    await page.waitForFunction(() => (window as any).__test.machine.state().frames > 130);
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
