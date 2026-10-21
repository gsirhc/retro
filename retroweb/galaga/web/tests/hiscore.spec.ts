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
    const buffer = Buffer.from(readFileSync(path));
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

// What POST ($0310) and $00D6 leave in RAM, built from the loaded main ROM: five copies of the 6-byte
// score at $02B9, five names from $02BF as c,$2A,c, then the HI-SCORE strip.
const FACTORY = (() => {
  const rom = readFileSync("roms/main.bin");
  rom[0] ^= 1;
  const score = [...rom.subarray(0x2b9, 0x2bf)];
  const out: number[] = [];
  for (let i = 0; i < 5; i++) out.push(...score);
  for (let i = 0; i < 5; i++) out.push(rom[0x2bf + i], 0x2a, rom[0x2bf + i]);
  out.push(...score.slice(0, 5), 0x24);
  return out;
})();

// The self-test ROM never runs the real POST, so plant the factory table until the restore lands.
async function postWritesFactoryTable(page: import("@playwright/test").Page) {
  await page.waitForFunction((factory) => {
    const t = (window as any).__test;
    if (!t.hiscoreRestored) t.writeHiscore(factory);
    return t.hiscoreRestored;
  }, FACTORY, { timeout: 15_000 });
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
    expect(await page.evaluate(() => (window as any).__test.hiscoreRestored)).toBe(false);

    await postWritesFactoryTable(page);
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
    expect(await page.evaluate(() => (window as any).__test.readHiscore())).not.toEqual(SAMPLE);
  });

  test("the restore waits for the whole factory table", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    await page.evaluate(async (bytes) => {
      const t = (window as any).__test;
      t.writeHiscore(bytes);
      await t.saveHiscoreNow(t.readHiscore());
    }, SAMPLE);

    await page.reload();
    await expect(page.locator("#romStatus")).toHaveText("Loaded stored ROM set");

    await page.waitForFunction(() => (window as any).__test?.usingUserRom);
    await page.evaluate((factory) => {
      const t = (window as any).__test;
      t.writeHiscore(factory.map((x: number, i: number) => (i === 20 ? x ^ 1 : x)));
    }, FACTORY);
    await page.waitForFunction(() => (window as any).__test.machine.state().frames > 120);
    expect(await page.evaluate(() => (window as any).__test.hiscoreRestored)).toBe(false);

    await postWritesFactoryTable(page);
    expect(await page.evaluate(() => (window as any).__test.readHiscore())).toEqual(SAMPLE);
  });
});
