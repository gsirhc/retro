import { test, expect } from "@playwright/test";
import { frames } from "./helpers";
import { readFileSync } from "fs";

// HIGH SCORE lives in work RAM at $2C-$5B (24 initials, 24 BCD). It persists for a user ROM and Reset clears it.
// The factory table is the ROM's own default, copied in after reset. Fixtures are the self-test ROM, which seeds
// a stand-in table the same way, so tests poke RAM through the ?test=1 seam.

const HWTEST_FILES = [
  "roms/035820-02.h1",
  "roms/035821-02.jk1",
  "roms/035822-03e.kl1",
  "roms/035823-02.lm1",
  "roms/035824-02.np1",
  "roms/035825-02.r1",
];

const HISCORE_LEN = 48;

// Shared IndexedDB origin, keep this file serial.
test.describe.configure({ mode: "serial" });

function userSet() {
  return HWTEST_FILES.map((path) => {
    const name = path.split("/").pop()!;
    let buffer = readFileSync(path);
    // Flip a byte in the unused tail of chip h1 so the CRC differs from hwtest.
    if (name === "035820-02.h1") {
      buffer = Buffer.from(buffer);
      buffer[0x7f0] ^= 1;
    }
    return { name, mimeType: "application/octet-stream" as const, buffer };
  });
}

function sampleBytes(seed: number) {
  const b = new Array(HISCORE_LEN).fill(0x41);
  b[24] = seed & 0xff;
  b[25] = (seed >> 8) & 0xff;
  return b;
}

const loadUser = async (page: import("@playwright/test").Page) => {
  await page.locator("#romFile").setInputFiles(userSet());
  await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
  await page.waitForFunction(() => (window as any).__test.hiscoreRestored);
};

test.describe("HIGH SCORE persist", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await page.waitForFunction(() => !!(window as any).__test?.machine);
    await page.evaluate(() => (window as any).__test.wipeStorage());
  });

  test("Reset is present and disabled on the self-test ROM", async ({ page }) => {
    const btn = page.locator("#resetHiscore");
    await expect(btn).toBeVisible();
    await expect(btn).toBeDisabled();
    await expect(page.locator("p.legal").filter({ hasText: "HIGH SCORE is kept" })).toBeVisible();
  });

  test("the table is 48 bytes and empty at power-on, then holds the ROM's default table", async ({ page }) => {
    const b = await page.evaluate(() => (window as any).__test.readHiscore());
    expect(b.length).toBe(HISCORE_LEN);
    await page.waitForFunction(() => (window as any).__test.machine.frames() > 200);
    const hw = readFileSync("roms/program.bin");
    const want = [...hw.subarray(0x24f3, 0x24f3 + 24), ...hw.subarray(0x250b, 0x250b + 24)];
    expect(await page.evaluate(() => (window as any).__test.readHiscore())).toEqual(want);
  });

  test("The table is saved and written back after a reload", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    await expect(page.locator("#resetHiscore")).toBeEnabled();

    const bytes = sampleBytes(0x76);
    await page.evaluate(async (b) => {
      const t = (window as any).__test;
      t.writeHiscore(b);
      await t.saveHiscoreNow(t.readHiscore());
    }, bytes);

    await page.reload();
    await expect(page.locator("#romStatus")).toHaveText("Loaded stored ROM set");
    await page.waitForFunction(() => (window as any).__test.hiscoreRestored);
    const restored = await page.evaluate(() => (window as any).__test.readHiscore());
    expect(restored).toEqual(bytes);

    const f0 = await page.evaluate(() => (window as any).__test.machine.frames());
    await page.waitForFunction((f) => (window as any).__test.machine.frames() > f + 120, f0);
    expect(await page.evaluate(() => (window as any).__test.readHiscore())).toEqual(bytes);
  });

  test("a byte the board writes to the table is saved without a manual call", async ({ page }) => {
    await loadUser(page);

    await page.evaluate(() => (window as any).__test.machine.setRamByte(0x30, 0x42));
    await expect.poll(() => page.evaluate(async () => {
      const t = (window as any).__test;
      const all = await t.loadSavedHiscores();
      return all[t.hiscoreKey]?.[4];
    })).toBe(0x42);
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
    await loadUser(page);
    const bytes = sampleBytes(0x50);
    await page.evaluate(async (b) => {
      const t = (window as any).__test;
      t.writeHiscore(b);
      await t.saveHiscoreNow(t.readHiscore());
    }, bytes);

    await page.locator("#resetHiscore").click();
    await expect(page.locator("#hiscoreResetHint")).toBeVisible();
    await page.locator("#hiscoreResetCancel").click();
    await expect(page.locator("#hiscoreResetHint")).toBeHidden();

    const after = await page.evaluate(() => (window as any).__test.readHiscore());
    expect(after).toEqual(bytes);
  });

  test("Reset confirm restores the factory table and drops the saved one", async ({ page }) => {
    await loadUser(page);
    const bytes = sampleBytes(0x76);
    await page.evaluate(async (b) => {
      const t = (window as any).__test;
      t.writeHiscore(b);
      await t.saveHiscoreNow(t.readHiscore());
    }, bytes);

    await page.locator("#resetHiscore").click();
    await page.locator("#hiscoreResetOk").click();
    await expect(page.locator("#hiscoreResetHint")).toBeHidden();

    await expect.poll(() => page.evaluate(async () => {
      const t = (window as any).__test;
      const all = await t.loadSavedHiscores();
      return { factory: t.readHiscore().join() === t.factoryHiscore().join(), saved: all[t.hiscoreKey] };
    })).toEqual({ factory: true, saved: undefined });

    await page.reload();
    await expect(page.locator("#romStatus")).toHaveText("Loaded stored ROM set");
    await page.waitForFunction(() => (window as any).__test.hiscoreRestored);
    const after = await page.evaluate(() => { const t = (window as any).__test; return t.readHiscore().join() === t.factoryHiscore().join(); });
    expect(after).toBe(true);
  });

  test("Reset restarts the board", async ({ page }) => {
    await loadUser(page);
    await page.waitForFunction(() => (window as any).__test.machine.frames() > 20);
    await page.evaluate(() => (window as any).__test.resetHiscoreNow());
    const frames = await page.evaluate(() => (window as any).__test.machine.frames());
    expect(frames).toBeLessThan(20);
  });

  test("the factory table is never saved", async ({ page }) => {
    await loadUser(page);
    const result = await page.evaluate(async () => {
      const t = (window as any).__test;
      await t.maybeSaveHiscore();
      const all = await t.loadSavedHiscores();
      return all[t.hiscoreKey];
    });
    expect(result).toBeUndefined();
  });

  test("the self-test ROM never touches the saved tables", async ({ page }) => {
    await page.evaluate(() => (window as any).__test.machine.setRamByte(0x30, 0x11));
    await frames(page, 18);
    const all = await page.evaluate(() => (window as any).__test.loadSavedHiscores());
    expect(Object.keys(all)).toEqual([]);
  });
});
