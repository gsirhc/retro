import { test, expect } from "@playwright/test";
import { readFileSync } from "fs";

// HIGH SCORE lives in the 64-byte EAROM. It persists for a user ROM and Reset EAROM clears it.
// Fixtures are the self-test ROM, so tests poke the EAROM through the ?test=1 seam.

const HWTEST_FILES = [
  "roms/136001-307.d1",
  "roms/136001-308.e1",
  "roms/136001-309.fh1",
  "roms/136001-310.j1",
  "roms/136001-211.f7",
  "roms/136001-212.hj7",
];

const HISCORE_LEN = 64;

// Shared IndexedDB origin, keep this file serial.
test.describe.configure({ mode: "serial" });

function userSet() {
  return HWTEST_FILES.map((path) => {
    const name = path.split("/").pop()!;
    let buffer = readFileSync(path);
    // Flip a byte in the unused tail of chip 307 so the CRC differs from hwtest.
    if (name === "136001-307.d1") {
      buffer = Buffer.from(buffer);
      buffer[0x7f0] ^= 1;
    }
    return { name, mimeType: "application/octet-stream" as const, buffer };
  });
}

function sampleBytes(seed: number) {
  const b = new Array(HISCORE_LEN).fill(0);
  b[0] = seed & 0xff;
  b[1] = (seed >> 8) & 0xff;
  return b;
}

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

  test("the EAROM image is 64 bytes and zero at power-on", async ({ page }) => {
    await page.waitForFunction(() => !!(window as any).__test?.machine);
    const b = await page.evaluate(() => (window as any).__test.readHiscore());
    expect(b.length).toBe(HISCORE_LEN);
    expect(b.every((v: number) => v === 0)).toBe(true);
  });

  test("EAROM is saved and written back after a reload", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    await expect(page.locator("#resetHiscore")).toBeEnabled();
    await page.waitForFunction(() => (window as any).__test.hiscoreRestored);

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
  });

  test("a byte the board writes to the EAROM is saved without a manual call", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    await page.waitForFunction(() => (window as any).__test.hiscoreRestored);

    await page.evaluate(() => (window as any).__test.machine.setEaromByte(5, 0x42));
    await expect.poll(() => page.evaluate(async () => {
      const t = (window as any).__test;
      const all = await t.loadSavedHiscores();
      return all[t.hiscoreKey]?.[5];
    })).toBe(0x42);
  });

  test("Remove ROMs disables Reset again", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#resetHiscore")).toBeEnabled();
    await page.locator("#removeRomBtn").click();
    await expect(page.locator("#romStatus")).toHaveText("Running test ROM");
    await expect(page.locator("#resetHiscore")).toBeDisabled();
  });

  test("Reset Cancel leaves the saved table in place", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    await page.waitForFunction(() => (window as any).__test.hiscoreRestored);
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

  test("Reset confirm clears the EAROM and the saved table", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    await page.waitForFunction(() => (window as any).__test.hiscoreRestored);
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
      return { zero: t.readHiscore().every((v: number) => v === 0), saved: all[t.hiscoreKey] };
    })).toEqual({ zero: true, saved: undefined });

    await page.reload();
    await expect(page.locator("#romStatus")).toHaveText("Loaded stored ROM set");
    await page.waitForFunction(() => (window as any).__test.hiscoreRestored);
    const after = await page.evaluate(() => (window as any).__test.readHiscore());
    expect(after.every((v: number) => v === 0)).toBe(true);
  });

  test("Reset restarts the board", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    await page.waitForFunction(() => (window as any).__test.hiscoreRestored);
    await page.waitForFunction(() => (window as any).__test.machine.frames() > 20);
    await page.evaluate(() => (window as any).__test.resetHiscoreNow());
    const frames = await page.evaluate(() => (window as any).__test.machine.frames());
    expect(frames).toBeLessThan(20);
  });

  test("factory-zero EAROM is never saved", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    await page.waitForFunction(() => (window as any).__test.hiscoreRestored);
    const result = await page.evaluate(async () => {
      const t = (window as any).__test;
      await t.maybeSaveHiscore();
      const all = await t.loadSavedHiscores();
      return all[t.hiscoreKey];
    });
    expect(result).toBeUndefined();
  });

  test("the self-test ROM never touches the saved tables", async ({ page }) => {
    await page.evaluate(() => (window as any).__test.machine.setEaromByte(0, 0x11));
    await page.waitForTimeout(300);
    const all = await page.evaluate(() => (window as any).__test.loadSavedHiscores());
    expect(Object.keys(all)).toEqual([]);
  });
});
