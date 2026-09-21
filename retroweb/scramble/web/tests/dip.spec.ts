import { test, expect } from "@playwright/test";

test.describe("DIP switches", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await page.waitForFunction(() => (window as any).__test);
  });

  test("factory default is 3 lives, 1C/1C, upright", async ({ page }) => {
    await expect(page.locator("#dipPanel")).toBeVisible();
    await expect(page.locator("#dipLives")).toHaveValue("0");
    await expect(page.locator("#dipCoinage")).toHaveValue("0");
    await expect(page.locator("#dipCabinet")).toHaveValue("0");
    expect(await page.evaluate(() => (window as any).__test.machine.in1())).toBe(0xFC);
    expect(await page.evaluate(() => (window as any).__test.machine.in2())).toBe(0x51);
    await expect(page.locator("#dipPanel .legal")).toContainText(/physical switches/i);
  });

  test("4 lives writes IN1 bits 1:0 = 01", async ({ page }) => {
    await page.locator("#dipLives").selectOption("1");
    expect(await page.evaluate(() => (window as any).__test.machine.in1())).toBe(0xFD);
  });

  test("A 1/2 writes IN2 bits 2:1 = 01", async ({ page }) => {
    await page.locator("#dipCoinage").selectOption("2");
    expect(await page.evaluate(() => (window as any).__test.machine.in2())).toBe(0x53);
  });

  test("cocktail cabinet sets IN2 bit 3", async ({ page }) => {
    await page.locator("#dipCabinet").selectOption("8");
    expect(await page.evaluate(() => (window as any).__test.machine.in2())).toBe(0x59);
  });

  test("a DIP change survives a reload", async ({ page }) => {
    await page.locator("#dipLives").selectOption("1");
    await page.locator("#dipCoinage").selectOption("6");
    await page.reload();
    await page.waitForFunction(() => (window as any).__test);
    await expect(page.locator("#dipLives")).toHaveValue("1");
    await expect(page.locator("#dipCoinage")).toHaveValue("6");
    expect(await page.evaluate(() => (window as any).__test.machine.in1())).toBe(0xFD);
    expect(await page.evaluate(() => (window as any).__test.machine.in2())).toBe(0x57);
  });
});
