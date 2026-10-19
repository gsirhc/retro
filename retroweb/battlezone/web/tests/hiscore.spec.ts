import { test, expect, type Page } from "@playwright/test";
import { readFileSync } from "fs";
import { HWTEST_FILES, USER_BYTE } from "./cabinet";

// HSCTBL at $0300 (ten 3-byte scores, then initials) persists for a user ROM. Reset clears it.
// The self-test ROM never runs Atari's power-on code, so tests write the factory pattern themselves.

const LEN = 0x3c;

// Shared IndexedDB origin, keep this file serial.
test.describe.configure({ mode: "serial" });

function userSet() {
  return HWTEST_FILES.map((path, i) => {
    const buffer = Buffer.from(readFileSync(path));
    if (i === 0) buffer[USER_BYTE] ^= 1;
    return { name: path.split("/").pop()!, mimeType: "application/octet-stream" as const, buffer };
  });
}

function factory() {
  const b = new Array(LEN).fill(0);
  for (let i = 0; i < 30; i += 3) b[i] = 5;
  return b;
}

function sample(seed: number) {
  const b = factory();
  b[1] = seed & 0xff;
  b[2] = (seed >> 8) & 0xff;
  b[30] = 0x0a;
  return b;
}

async function powerOn(page: Page) {
  await page.waitForFunction(() => (window as any).__test.machine.frames() > 20);
  await page.evaluate((b) => (window as any).__test.writeHiscore(b), factory());
}
const save = async (page: Page, bytes: number[]) => {
  await page.waitForFunction(() => (window as any).__test.machine.frames() > 20);
  await page.evaluate(async (b) => {
    const t = (window as any).__test;
    t.writeHiscore(b);
    await t.saveHiscoreNow(t.readHiscore());
  }, bytes);
};
const restored = (page: Page) => page.evaluate(() => (window as any).__test.hiscoreRestored);
const read = (page: Page) => page.evaluate(() => (window as any).__test.readHiscore());

async function loadUser(page: Page) {
  await page.locator("#romFile").setInputFiles(userSet());
  await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
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

  test("the table is 60 bytes", async ({ page }) => {
    expect(await read(page)).toHaveLength(LEN);
  });

  test("the factory check wants a 5 in the first byte of each of ten scores", async ({ page }) => {
    const is = (b: number[]) => page.evaluate((x) => (window as any).__test.hiscoreIsFactory(x), b);
    expect(await is(factory())).toBe(true);
    expect(await is(sample(0x1234))).toBe(false);
    const bad = factory();
    bad[27] = 4;
    expect(await is(bad)).toBe(false);
    expect(await is(new Array(LEN).fill(0))).toBe(false);
  });

  test("the table is saved and written back after a reload", async ({ page }) => {
    await loadUser(page);
    await expect(page.locator("#resetHiscore")).toBeEnabled();
    const bytes = sample(0x1234);
    await save(page, bytes);

    await page.reload();
    await expect(page.locator("#romStatus")).toHaveText("Loaded stored ROM set");
    await page.waitForFunction(() => !!(window as any).__test?.machine);
    await powerOn(page);
    await expect.poll(() => restored(page)).toBe(true);
    expect(await read(page)).toEqual(bytes);
  });

  test("the restore waits until the table looks factory", async ({ page }) => {
    await loadUser(page);
    const bytes = sample(0x1234);
    await save(page, bytes);

    await page.reload();
    await expect(page.locator("#romStatus")).toHaveText("Loaded stored ROM set");
    await page.waitForFunction(() => !!(window as any).__test?.machine);
    await page.evaluate(() => (window as any).__test.writeHiscore(new Array(0x3c).fill(0x11)));
    await page.waitForFunction(() => (window as any).__test.machine.frames() > 360);
    expect(await restored(page)).toBe(false);
    expect(await page.evaluate(() => (window as any).__test.hiscoreReady())).toBe(false);

    await powerOn(page);
    await expect.poll(() => restored(page)).toBe(true);
    expect(await read(page)).toEqual(bytes);
  });

  test("a byte the board writes to the table is saved without a manual call", async ({ page }) => {
    await loadUser(page);
    await powerOn(page);
    await expect.poll(() => restored(page)).toBe(true);

    await page.evaluate(() => (window as any).__test.machine.setRamByte(0x301, 0x42));
    await expect.poll(() => page.evaluate(async () => {
      const t = (window as any).__test;
      const all = await t.loadSavedHiscores();
      return all[t.hiscoreKey]?.[1];
    })).toBe(0x42);
  });

  test("Remove ROMs disables Reset again", async ({ page }) => {
    await loadUser(page);
    await expect(page.locator("#resetHiscore")).toBeEnabled();
    await page.locator("#removeRomBtn").click();
    await expect(page.locator("#romStatus")).toHaveText("Running test ROM");
    await page.waitForFunction(() => !!(window as any).__test?.machine);
    await expect(page.locator("#resetHiscore")).toBeDisabled();
  });

  test("Reset Cancel leaves the saved table in place", async ({ page }) => {
    await loadUser(page);
    const bytes = sample(0x0050);
    await save(page, bytes);

    await page.locator("#resetHiscore").click();
    await expect(page.locator("#hiscoreResetHint")).toBeVisible();
    await page.locator("#hiscoreResetCancel").click();
    await expect(page.locator("#hiscoreResetHint")).toBeHidden();

    expect(await read(page)).toEqual(bytes);
    expect(Object.keys(await page.evaluate(() => (window as any).__test.savedHiscores()))).toHaveLength(1);
  });

  test("Reset confirm clears the saved table", async ({ page }) => {
    await loadUser(page);
    await save(page, sample(0x0076));
    expect(Object.keys(await page.evaluate(() => (window as any).__test.savedHiscores()))).toHaveLength(1);

    await page.locator("#resetHiscore").click();
    await page.locator("#hiscoreResetOk").click();
    await expect(page.locator("#hiscoreResetHint")).toBeHidden();

    await expect.poll(async () => Object.keys(await page.evaluate(() => (window as any).__test.savedHiscores()))).toHaveLength(0);
  });

  test("Reset restarts the board", async ({ page }) => {
    await loadUser(page);
    await page.waitForFunction(() => (window as any).__test.machine.frames() > 40);
    await page.evaluate(() => (window as any).__test.resetHiscoreNow());
    expect(await page.evaluate(() => (window as any).__test.machine.frames())).toBeLessThan(40);
  });

  test("the factory table is never saved", async ({ page }) => {
    await loadUser(page);
    await powerOn(page);
    await expect.poll(() => restored(page)).toBe(true);
    const result = await page.evaluate(async () => {
      const t = (window as any).__test;
      await t.maybeSaveHiscore();
      const all = await t.loadSavedHiscores();
      return all[t.hiscoreKey];
    });
    expect(result).toBeUndefined();
  });

  test("the self-test ROM never touches the saved tables", async ({ page }) => {
    await page.evaluate(() => (window as any).__test.machine.setRamByte(0x301, 0x11));
    await page.waitForFunction(() => (window as any).__test.machine.frames() > 60);
    const all = await page.evaluate(() => (window as any).__test.loadSavedHiscores());
    expect(Object.keys(all)).toEqual([]);
  });
});
