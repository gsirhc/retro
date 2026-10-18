import { test, expect } from "@playwright/test";

const in2 = (page: import("@playwright/test").Page) =>
  page.evaluate(() => (window as any).__test.machine.in2());

test.describe("DIP switches", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await page.waitForFunction(() => (window as any).__test);
  });

  test("factory default is 3 lives, extra base at 1500, coin info shown", async ({ page }) => {
    await expect(page.locator("#dipPanel")).toBeVisible();
    await expect(page.locator("#dipLives")).toHaveValue("0");
    await expect(page.locator("#dipBonus")).toHaveValue("0");
    await expect(page.locator("#dipCoinInfo")).toHaveValue("0");
    expect(await in2(page)).toBe(0x00);
    await expect(page.locator("#dipPanel .legal")).toContainText(/just like on the real arcade cabinet/i);
  });

  test("lives write IN2 bits 1:0", async ({ page }) => {
    for (const [v, bits] of [["1", 0x01], ["2", 0x02], ["3", 0x03]] as const) {
      await page.locator("#dipLives").selectOption({ value: v });
      expect(await in2(page)).toBe(bits);
    }
  });

  test("extra base at 1000 sets IN2 bit 3", async ({ page }) => {
    await page.locator("#dipBonus").selectOption("8");
    expect(await in2(page)).toBe(0x08);
  });

  test("hiding the coin info sets IN2 bit 7", async ({ page }) => {
    await page.locator("#dipCoinInfo").selectOption("128");
    expect(await in2(page)).toBe(0x80);
  });

  test("a DIP change survives a reload", async ({ page }) => {
    await page.locator("#dipLives").selectOption({ value: "3" });
    await page.locator("#dipCoinInfo").selectOption("128");
    await page.reload();
    await page.waitForFunction(() => (window as any).__test);
    await expect(page.locator("#dipLives")).toHaveValue("3");
    await expect(page.locator("#dipCoinInfo")).toHaveValue("128");
    expect(await in2(page)).toBe(0x83);
  });
});
