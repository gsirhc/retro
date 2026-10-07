import { test, expect } from "@playwright/test";
import { readFileSync } from "fs";

// HIGH SCORE work RAM ($1d-$51) persists for a user ROM. Reset HIGH SCORE clears it.
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
  const b = new Array(0x35).fill(0);
  b[0] = seed & 0xff;
  b[1] = (seed >> 8) & 0xff;
  return b;
}

test.describe("HIGH SCORE persist", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
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

    const restored = await page.evaluate(async () => {
      await (window as any).__test.restoreHiscoreNow();
      return (window as any).__test.readHiscore();
    });
    expect(restored).toEqual(bytes);
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

    await page.locator("#resetHiscore").click();
    await page.locator("#hiscoreResetOk").click();
    await expect(page.locator("#hiscoreResetHint")).toBeHidden();

    const probe = sampleBytes(0x99);
    const after = await page.evaluate(async (b) => {
      const t = (window as any).__test;
      await t.resetHiscoreNow();
      t.writeHiscore(b);
      await t.restoreHiscoreNow();
      return t.readHiscore();
    }, probe);
    expect(after).toEqual(probe);
  });

  test("attract rank bytes do not block persist", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    const bytes = sampleBytes(0x70);
    const restored = await page.evaluate(async (b) => {
      const t = (window as any).__test;
      const cur = t.readHiscore();
      cur[0x15] = 0;
      cur[0x16] = 0xff;
      cur[0x17] = 0xff;
      t.writeHiscore(cur);
      for (let i = 0; i < 400 && !t.hiscoreRestored; i++) {
        await new Promise((r) => requestAnimationFrame(r));
      }
      if (!t.hiscoreRestored) return { restored: false, table: t.readHiscore() };
      t.writeHiscore(b);
      await t.maybeSaveHiscore();
      return { restored: true, table: t.readHiscore() };
    }, bytes);
    expect(restored.restored).toBe(true);

    await page.reload();
    await expect(page.locator("#romStatus")).toHaveText("Loaded stored ROM set");
    const after = await page.evaluate(async () => {
      const t = (window as any).__test;
      for (let i = 0; i < 400 && !t.hiscoreRestored; i++) {
        await new Promise((r) => requestAnimationFrame(r));
      }
      return { restored: t.hiscoreRestored, table: t.readHiscore() };
    });
    expect(after.restored).toBe(true);
    expect(after.table.slice(0, 2)).toEqual(bytes.slice(0, 2));
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
