import { test, expect } from "@playwright/test";
import { readFileSync } from "fs";

const HWTEST_FILES = [
  "roms/program.bin",
  "roms/sound.bin",
  "roms/gfx.bin",
  "roms/c01s.6e",
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

const SAMPLE = Array.from({ length: 0x21 }, (_, i) => (i + 1) & 0xff);
// What POST leaves in the table: $00 at $4200 and $01 at $421D open the restore gate.
const FACTORY = Array.from({ length: 0x21 }, (_, i) => (i === 0x1D ? 0x01 : 0x00));

async function postWritesFactoryTable(page: import("@playwright/test").Page) {
  await page.evaluate((bytes) => (window as any).__test.writeHiscore(bytes), FACTORY);
}

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

  test("top-10 table RAM is saved and poked back after a reload", async ({ page }) => {
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

    expect(await page.evaluate(() => (window as any).__test.hiscoreReady())).toBe(false);
    expect(await page.evaluate(() => (window as any).__test.hiscoreRestored)).toBe(false);

    await postWritesFactoryTable(page);
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

    await postWritesFactoryTable(page);
    await expect.poll(() => page.evaluate(() => (window as any).__test.hiscoreRestored)).toBe(true);
    expect(await page.evaluate(() => (window as any).__test.readHiscore())).toEqual(FACTORY);
  });
});
