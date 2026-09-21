import { test, expect } from "@playwright/test";

test.describe("DIP switches", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await page.waitForFunction(() => (window as any).__test);
  });

  test("factory default is 3 lives, 1C/1C, 7000 bonus, upright", async ({ page }) => {
    await expect(page.locator("#dipPanel")).toBeVisible();
    await expect(page.locator("#dipLives")).toHaveValue("4");
    await expect(page.locator("#dipCoinage")).toHaveValue("0");
    await expect(page.locator("#dipBonus")).toHaveValue("0");
    await expect(page.locator("#dipCabinet")).toHaveValue("0");
    expect(await page.evaluate(() => (window as any).__test.machine.in0())).toBe(0x00);
    expect(await page.evaluate(() => (window as any).__test.machine.in1())).toBe(0x00);
    expect(await page.evaluate(() => (window as any).__test.machine.in2())).toBe(0x04);
    await expect(page.locator("#dipPanel .legal")).toContainText(/physical switches/i);
  });

  test("2 lives writes IN2 bit 2 = 0", async ({ page }) => {
    await page.locator("#dipLives").selectOption("0");
    expect(await page.evaluate(() => (window as any).__test.machine.in2())).toBe(0x00);
  });

  test("1C/2C writes IN1 bits 7:6 = 01", async ({ page }) => {
    await page.locator("#dipCoinage").selectOption("64");
    expect(await page.evaluate(() => (window as any).__test.machine.in1())).toBe(0x40);
  });

  test("cocktail cabinet sets IN0 bit 5", async ({ page }) => {
    await page.locator("#dipCabinet").selectOption("32");
    expect(await page.evaluate(() => (window as any).__test.machine.in0())).toBe(0x20);
  });

  test("a DIP change survives a reload", async ({ page }) => {
    await page.locator("#dipLives").selectOption("0");
    await page.locator("#dipCoinage").selectOption("192");
    await page.reload();
    await page.waitForFunction(() => (window as any).__test);
    await expect(page.locator("#dipLives")).toHaveValue("0");
    await expect(page.locator("#dipCoinage")).toHaveValue("192");
    expect(await page.evaluate(() => (window as any).__test.machine.in1())).toBe(0xC0);
    expect(await page.evaluate(() => (window as any).__test.machine.in2())).toBe(0x00);
  });
});
