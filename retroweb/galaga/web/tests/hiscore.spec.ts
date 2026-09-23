import { test, expect } from "@playwright/test";
import { readFileSync } from "fs";

const PARTS = [
  "roms/main.bin",
  "roms/sub.bin",
  "roms/sound.bin",
  "roms/2600j.bin",
  "roms/sprites.bin",
  "roms/prom-5.5n",
  "roms/prom-4.2n",
  "roms/prom-3.1c",
  "roms/prom-1.1d",
];

function userSet() {
  const files = PARTS.map((path) => {
    const name = path.split("/").pop()!;
    let buffer = Buffer.from(readFileSync(path));
    if (name === "main.bin") buffer[0] ^= 1;
    return { name, mimeType: "application/octet-stream" as const, buffer };
  });
  files.push(
    { name: "51xx.bin", mimeType: "application/octet-stream", buffer: Buffer.alloc(0x400, 1) },
    { name: "54xx.bin", mimeType: "application/octet-stream", buffer: Buffer.alloc(0x400, 2) },
  );
  return files;
}

const SAMPLE = Array.from({ length: 51 }, (_, i) => (i === 4 ? 0x76 : i & 0xff));
const SENTINEL = Array.from({ length: 51 }, () => 0x99);

test.describe("HIGH SCORE persist", () => {
  test.describe.configure({ mode: "serial" });
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
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

    const bytes = await page.evaluate(async () => {
      await (window as any).__test.restoreHiscoreNow();
      return (window as any).__test.readHiscore();
    });
    expect(bytes).toEqual(SAMPLE);
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

    await page.locator("#resetHiscore").click();
    await page.locator("#hiscoreResetOk").click();
    await expect(page.locator("#hiscoreResetHint")).toBeHidden();
    // Confirm resets the board asynchronously. The saved 0x76 byte goes
    // back to 0, then a couple of frames let the self-test fill finish.
    await expect.poll(() => page.evaluate(() => (window as any).__test.readHiscore()[4])).toBe(0);
    const frames = await page.evaluate(() => (window as any).__test.machine.state().frames);
    await expect.poll(() => page.evaluate(() => (window as any).__test.machine.state().frames)).toBeGreaterThan(frames + 2);

    const bytes = await page.evaluate(async (sent) => {
      const t = (window as any).__test;
      await t.restoreHiscoreNow();
      const afterRestore = t.readHiscore();
      t.writeHiscore(sent);
      return { afterRestore, afterPoke: t.readHiscore() };
    }, SENTINEL);
    expect(bytes.afterRestore).not.toEqual(SAMPLE);
    expect(bytes.afterPoke).toEqual(SENTINEL);
  });
});
