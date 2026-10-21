import { test, expect } from "@playwright/test";
import { readFileSync } from "fs";

// HIGH SCORE work RAM (scores $1d-$30, initials $34-$51) persists for a user ROM. Reset HIGH SCORE clears it.
// Fixtures are the self-test ROM, so tests poke the RAM through the ?test=1 seam.

const HWTEST_FILES = [
  "roms/035145-04e.ef2",
  "roms/035144-04e.h2",
  "roms/035143-02.j2",
  "roms/035127-02.np3",
];

// Shared IndexedDB origin, keep this file serial.
test.describe.configure({ mode: "serial" });

function userSet() {
  return HWTEST_FILES.map((path) => {
    const name = path.split("/").pop()!;
    let buffer = readFileSync(path);
    // Flip a byte in the unused zero padding. Byte 0 would break the watchdog loop.
    if (name === "035145-04e.ef2") {
      buffer = Buffer.from(buffer);
      buffer[1024] ^= 1;
    }
    return { name, mimeType: "application/octet-stream" as const, buffer };
  });
}

function sampleBytes(seed: number) {
  const b = new Array(50).fill(0);
  b[0] = seed & 0xff;
  b[1] = (seed >> 8) & 0xff;
  b[20] = 0x11;
  b[49] = 0x22;
  return b;
}

test.describe("HIGH SCORE persist", () => {
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

  test("work RAM is saved and poked back after a reload", async ({ page }) => {
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

    await expect.poll(() => page.evaluate(() => (window as any).__test.hiscoreRestored)).toBe(true);
    expect(await page.evaluate(() => (window as any).__test.readHiscore())).toEqual(bytes);
  });

  test("the restore waits until the startup RAM clear leaves the scores empty", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    const bytes = sampleBytes(0x76);
    await page.evaluate(async (b) => {
      const t = (window as any).__test;
      t.writeHiscore(b);
      await t.saveHiscoreNow(t.readHiscore());
    }, bytes);

    await page.reload();
    await expect(page.locator("#romStatus")).toHaveText("Loaded stored ROM set");
    const junk = sampleBytes(0x11);
    await page.evaluate((b) => (window as any).__test.writeHiscore(b), junk);
    await page.waitForFunction(() => (window as any).__test.machine.frames() > 360);
    expect(await page.evaluate(() => (window as any).__test.hiscoreRestored)).toBe(false);

    await page.evaluate(() => (window as any).__test.writeHiscore(new Array(50).fill(0)));
    await expect.poll(() => page.evaluate(() => (window as any).__test.hiscoreRestored)).toBe(true);
    expect(await page.evaluate(() => (window as any).__test.readHiscore())).toEqual(bytes);
  });

  test("the table spans scores $1d-$30 and initials $34-$51", async ({ page }) => {
    const idx = await page.evaluate(() => {
      const t = (window as any).__test;
      const at = (addr: number) => {
        const before = t.readHiscore();
        t.machine.setRamByte(addr, 0xa5);
        const after = t.readHiscore();
        t.machine.setRamByte(addr, 0);
        return after.findIndex((v: number, i: number) => v !== before[i]);
      };
      return [0x1d, 0x30, 0x31, 0x32, 0x33, 0x34, 0x51, 0x52].map(at);
    });
    expect(idx).toEqual([0, 19, -1, -1, -1, 20, 49, -1]);
  });

  test("a 53-byte $1d-$51 save restores without the $31-$33 entry state", async ({ page }) => {
    const run = new Array(0x35).fill(0).map((_, i) => i + 1);
    const got = await page.evaluate((b) => {
      const t = (window as any).__test;
      t.writeHiscore(b);
      return [t.readHiscore(), [0x31, 0x32, 0x33].map((a: number) => t.machine.ramByte(a) & 0xff)];
    }, run);
    expect(got[0]).toEqual([...run.slice(0, 20), ...run.slice(23)]);
    expect(got[1]).toEqual([0, 0, 0]);
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

  test("Reset confirm clears the saved table", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    const bytes = sampleBytes(0x76);
    await page.evaluate(async (b) => {
      const t = (window as any).__test;
      t.writeHiscore(b);
      await t.saveHiscoreNow(t.readHiscore());
    }, bytes);

    expect(Object.keys(await page.evaluate(() => (window as any).__test.savedHiscores()))).toHaveLength(1);

    await page.locator("#resetHiscore").click();
    await page.locator("#hiscoreResetOk").click();
    await expect(page.locator("#hiscoreResetHint")).toBeHidden();

    await expect.poll(async () => Object.keys(await page.evaluate(() => (window as any).__test.savedHiscores()))).toHaveLength(0);
    await expect.poll(() => page.evaluate(() => (window as any).__test.hiscoreRestored)).toBe(true);
    expect((await page.evaluate(() => (window as any).__test.readHiscore())).slice(0, 20)).toEqual(Array(20).fill(0));
  });

  test("initials-entry state at $31-$33 is not part of the saved table", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    const bytes = sampleBytes(0x70);
    const out = await page.evaluate(async (b) => {
      const t = (window as any).__test;
      for (const a of [0x31, 0x32, 0x33]) t.machine.setRamByte(a, 0xff);
      for (let i = 0; i < 400 && !t.hiscoreRestored; i++) {
        await new Promise((r) => requestAnimationFrame(r));
      }
      t.writeHiscore(b);
      await t.maybeSaveHiscore();
      return { restored: t.hiscoreRestored, len: t.readHiscore().length, state: [0x31, 0x32, 0x33].map((a) => t.machine.ramByte(a)) };
    }, bytes);
    expect(out.restored).toBe(true);
    expect(out.len).toBe(50);
    expect(out.state).toEqual([0xff, 0xff, 0xff]);

    await page.reload();
    await expect(page.locator("#romStatus")).toHaveText("Loaded stored ROM set");
    const after = await page.evaluate(async () => {
      const t = (window as any).__test;
      for (let i = 0; i < 400 && !t.hiscoreRestored; i++) {
        await new Promise((r) => requestAnimationFrame(r));
      }
      return { restored: t.hiscoreRestored, table: t.readHiscore(), state: [0x31, 0x32, 0x33].map((a) => t.machine.ramByte(a)) };
    });
    expect(after.restored).toBe(true);
    expect(after.table.slice(0, 2)).toEqual(bytes.slice(0, 2));
    expect(after.state).not.toEqual([0xff, 0xff, 0xff]);
  });

  test("factory-zero RAM is never saved", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    const result = await page.evaluate(async () => {
      const t = (window as any).__test;
      await t.maybeSaveHiscore();
      const all = await t.loadSavedHiscores();
      return all[t.hiscoreKey];
    });
    expect(result).toBeUndefined();
  });
});
