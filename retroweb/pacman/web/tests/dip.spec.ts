import { test, expect } from "@playwright/test";

// DIP panel is a labelled stand-in for the cabinet's operator bank.
// Persistence is authentic: physical switches survive a power cycle.
// Bit map: PACMAN_REVIEW.md §7 / Inputs in machine.h.
// Factory DSW1 is 0xC9 (1C/1C, 3 lives, bonus 10k, normal, normal names).

test.describe("DIP switches", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await page.waitForFunction(() => (window as any).__test);
  });

  test("factory default is 1C/1C, 3 lives, bonus 10k (DSW1 = 0xC9)", async ({ page }) => {
    await expect(page.locator("#dipPanel")).toBeVisible();
    await expect(page.locator("#dipCoinage")).toHaveValue("1");
    await expect(page.locator("#dipLives")).toHaveValue("8");
    await expect(page.locator("#dipBonus")).toHaveValue("0");
    await expect(page.locator("#dipDifficulty")).toHaveValue("64");
    await expect(page.locator("#dipGhosts")).toHaveValue("128");
    await expect(page.locator("#dipCabinet")).toHaveValue("0");
    await expect(page.locator("#dipRack")).toHaveValue("0");
    expect(await page.evaluate(() => (window as any).__test.machine.dsw1())).toBe(0xC9);
    await expect(page.locator("#dipPanel .legal")).toContainText(/physical switches/i);
  });

  const dsw1Cases: [string, string, string, number][] = [
    ["coinage free play", "#dipCoinage", "0", 0xC8],
    ["coinage 2C/1C", "#dipCoinage", "3", 0xCB],
    ["coinage 1C/2C", "#dipCoinage", "2", 0xCA],
    ["1 life", "#dipLives", "0", 0xC1],
    ["2 lives", "#dipLives", "4", 0xC5],
    ["5 lives", "#dipLives", "12", 0xCD],
    ["bonus 15k", "#dipBonus", "16", 0xD9],
    ["bonus 20k", "#dipBonus", "32", 0xE9],
    ["bonus none", "#dipBonus", "48", 0xF9],
    ["hard difficulty", "#dipDifficulty", "0", 0x89],
    ["alternate ghost names", "#dipGhosts", "0", 0x49],
  ];
  for (const [name, sel, value, dsw1] of dsw1Cases) {
    test(`${name} writes DSW1 = 0x${dsw1.toString(16)}`, async ({ page }) => {
      await page.locator(sel).selectOption(value);
      expect(await page.evaluate(() => (window as any).__test.machine.dsw1())).toBe(dsw1);
    });
  }

  test("a DSW1 change survives a reload", async ({ page }) => {
    await page.locator("#dipLives").selectOption("12");
    await page.locator("#dipCoinage").selectOption("0");
    expect(await page.evaluate(() => (window as any).__test.machine.dsw1())).toBe(0xCC);

    await page.reload();
    await page.waitForFunction(() => (window as any).__test);
    await expect(page.locator("#dipLives")).toHaveValue("12");
    await expect(page.locator("#dipCoinage")).toHaveValue("0");
    expect(await page.evaluate(() => (window as any).__test.machine.dsw1())).toBe(0xCC);
  });

  test("rack test clears IN0 bit 0x10 and survives a reload", async ({ page }) => {
    expect(await page.evaluate(() => (window as any).__test.machine.in0())).toBe(0xFF);
    await page.locator("#dipRack").selectOption("1");
    expect(await page.evaluate(() => (window as any).__test.machine.in0())).toBe(0xFF & ~0x10);

    await page.reload();
    await page.waitForFunction(() => (window as any).__test);
    await expect(page.locator("#dipRack")).toHaveValue("1");
    expect(await page.evaluate(() => (window as any).__test.machine.in0())).toBe(0xFF & ~0x10);
  });

  test("cocktail cabinet clears IN1 bit 0x80 and survives a reload", async ({ page }) => {
    expect(await page.evaluate(() => (window as any).__test.machine.in1())).toBe(0xFF);
    await page.locator("#dipCabinet").selectOption("1");
    expect(await page.evaluate(() => (window as any).__test.machine.in1())).toBe(0xFF & ~0x80);

    await page.reload();
    await page.waitForFunction(() => (window as any).__test);
    await expect(page.locator("#dipCabinet")).toHaveValue("1");
    expect(await page.evaluate(() => (window as any).__test.machine.in1())).toBe(0xFF & ~0x80);
  });
});
