import { test, expect } from "@playwright/test";
import { readFileSync } from "fs";

const HWTEST_FILES = [
  "roms/program.bin",
  "roms/sound.bin",
  "roms/gfx.bin",
  "roms/pr-91.6l",
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

const SAMPLE = [0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc];
// What POST leaves at $83EF: $63 at $83F1 and $01 at $83FA open the restore gate.
const FACTORY = [0, 0, 0x63, 0, 0, 0, 0, 0, 0, 0, 0, 0x01];

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

  test("ranking RAM is saved and poked back after a reload", async ({ page }) => {
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
    await expect(page.locator("#resetHiscore")).toBeDisabled();
  });

  // Swaps in the self-test set while a save or reset is waiting on IndexedDB.
  async function swapSetDuring(page: import("@playwright/test").Page, action: "save" | "reset") {
    return page.evaluate(async ([act, bytes]) => {
      const t = (window as any).__test;
      const files: Record<string, Uint8Array> = {};
      for (const n of ["program.bin", "sound.bin", "gfx.bin", "pr-91.6l"]) {
        files[n] = new Uint8Array(await (await fetch("roms/" + n)).arrayBuffer());
      }
      const hwtest = t.identifySet(files, t.hwtestCrcs);
      const user = files["program.bin"].slice();
      user[0] ^= 1;
      const userKey = String(t.crc32(user));
      if (act === "reset") {
        t.writeHiscore(bytes);
        await t.saveHiscoreNow(t.readHiscore());
      }
      t.writeHiscore(bytes);
      const pending = act === "save" ? t.saveHiscoreNow(t.readHiscore()) : t.resetHiscoreNow();
      t.applySet(hwtest, "Hardware test ROM");
      await pending;
      return { userKey, saved: Object.keys(await t.savedHiscores()) };
    }, [action, SAMPLE] as const);
  }

  test("a save in flight when the ROM set changes is filed under the old set", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    const r = await swapSetDuring(page, "save");
    expect(r.saved).toEqual([r.userKey]);
  });

  test("a reset in flight when the ROM set changes clears the old set's table", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    const r = await swapSetDuring(page, "reset");
    expect(r.saved).toEqual([]);
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
