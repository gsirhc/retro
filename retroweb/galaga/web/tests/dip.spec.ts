import { test, expect } from "@playwright/test";

test.describe("DIP switches", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await page.waitForFunction(() => (window as any).__test);
  });

  const dsw = (page: import("@playwright/test").Page) =>
    page.evaluate(() => ({
      a: (window as any).__test.machine.dswA(),
      b: (window as any).__test.machine.dswB(),
    }));

  test("factory default is 2-player credits, easy, demo on, 1C/1C, 20K/70K, 3 lives, upright", async ({ page }) => {
    await expect(page.locator("#dipPanel")).toBeVisible();
    await expect(page.locator("#dipCredits")).toHaveValue("1");
    await expect(page.locator("#dipDifficulty")).toHaveValue("6");
    await expect(page.locator("#dipDemo")).toHaveValue("0");
    await expect(page.locator("#dipFreeze")).toHaveValue("16");
    await expect(page.locator("#dipRack")).toHaveValue("32");
    await expect(page.locator("#dipCabinet")).toHaveValue("128");
    await expect(page.locator("#dipCoinage")).toHaveValue("7");
    await expect(page.locator("#dipBonus")).toHaveValue("16");
    await expect(page.locator("#dipLives")).toHaveValue("128");
    expect(await dsw(page)).toEqual({ a: 0xF7, b: 0x97 });
    await expect(page.locator("#dipPanel .legal")).toContainText(/physical switches/i);
  });

  test("1-player 2-credit game clears SWB bit 0", async ({ page }) => {
    await page.locator("#dipCredits").selectOption("0");
    expect((await dsw(page)).a).toBe(0xF6);
  });

  test("medium, hard, and hardest write the difficulty pair", async ({ page }) => {
    await page.locator("#dipDifficulty").selectOption("0");
    expect((await dsw(page)).a).toBe(0xF1);
    await page.locator("#dipDifficulty").selectOption("2");
    expect((await dsw(page)).a).toBe(0xF3);
    await page.locator("#dipDifficulty").selectOption("4");
    expect((await dsw(page)).a).toBe(0xF5);
  });

  test("demo sounds off sets SWB bit 3", async ({ page }) => {
    await page.locator("#dipDemo").selectOption("8");
    expect((await dsw(page)).a).toBe(0xFF);
  });

  test("freeze on clears SWB bit 4", async ({ page }) => {
    await page.locator("#dipFreeze").selectOption("0");
    expect((await dsw(page)).a).toBe(0xE7);
  });

  test("rack test on clears SWB bit 5", async ({ page }) => {
    await page.locator("#dipRack").selectOption("0");
    expect((await dsw(page)).a).toBe(0xD7);
  });

  test("cocktail cabinet clears SWB bit 7", async ({ page }) => {
    await page.locator("#dipCabinet").selectOption("0");
    expect((await dsw(page)).a).toBe(0x77);
  });

  test("each coinage value lands in SWA bits 2:0", async ({ page }) => {
    for (const value of ["4", "2", "6", "7", "1", "3", "5", "0"]) {
      await page.locator("#dipCoinage").selectOption(value);
      expect((await dsw(page)).b & 0x07).toBe(Number(value));
    }
  });

  test("each bonus value lands in SWA bits 5:3", async ({ page }) => {
    for (const value of ["32", "24", "16", "48", "56", "8", "40", "0"]) {
      await page.locator("#dipBonus").selectOption(value);
      expect((await dsw(page)).b & 0x38).toBe(Number(value));
    }
  });

  test("2, 4, and 5 lives write SWA bits 7:6", async ({ page }) => {
    await page.locator("#dipLives").selectOption("0");
    expect((await dsw(page)).b & 0xC0).toBe(0);
    await page.locator("#dipLives").selectOption("64");
    expect((await dsw(page)).b & 0xC0).toBe(0x40);
    await page.locator("#dipLives").selectOption("192");
    expect((await dsw(page)).b & 0xC0).toBe(0xC0);
  });

  test("a DIP change survives a reload", async ({ page }) => {
    await page.locator("#dipLives").selectOption("0");
    await page.locator("#dipCoinage").selectOption("4");
    await page.locator("#dipCabinet").selectOption("0");
    await page.reload();
    await page.waitForFunction(() => (window as any).__test);
    await expect(page.locator("#dipLives")).toHaveValue("0");
    await expect(page.locator("#dipCoinage")).toHaveValue("4");
    await expect(page.locator("#dipCabinet")).toHaveValue("0");
    expect(await dsw(page)).toEqual({ a: 0x77, b: 0x14 });
  });
});
