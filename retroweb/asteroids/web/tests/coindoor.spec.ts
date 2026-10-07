import { test, expect } from "@playwright/test";

// Coin slots pulse IN1 bit 0, same as the 5 key. Center and right stay on 6 and 7.

test.describe("coin door", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await expect(page.locator("#coinDoor")).toBeVisible();
    await expect.poll(() => page.evaluate(() => !!(window as any).__test)).toBe(true);
  });

  const in1 = (page: import("@playwright/test").Page) =>
    page.evaluate(() => (window as any).__test.machine.in1());

  test("shows two 25¢ slots and the mute control", async ({ page }) => {
    await expect(page.locator(".cab-face")).toBeVisible();
    await expect(page.locator("#coinDoor [data-coin]")).toHaveCount(2);
    await expect(page.locator(".cd-lamp")).toHaveCount(2);
    await expect(page.locator(".cd-lamp").first()).toHaveText("25¢");
    await expect(page.locator("#mute")).toBeVisible();
  });

  test("shows 1 PLAYER and 2 PLAYER start buttons above the slots", async ({ page }) => {
    await expect(page.locator("[data-start]")).toHaveCount(2);
    await expect(page.locator("[data-start='1']")).toHaveAttribute("aria-label", "1 Player start");
    await expect(page.locator("[data-start='2']")).toHaveAttribute("aria-label", "2 Player start");
    await expect(page.locator("[data-start='1'] .cab-start-img")).toBeVisible();
    await expect(page.locator("[data-start='2'] .cab-start-img")).toBeVisible();
  });

  test("holding 1 PLAYER sets IN1 bit 0x08, then releases", async ({ page }) => {
    expect(await in1(page)).toBe(0);
    const btn = page.locator("[data-start='1']");
    await btn.dispatchEvent("pointerdown", { button: 0, pointerId: 1 });
    await expect.poll(() => in1(page)).toBe(0x08);
    await btn.dispatchEvent("pointerup", { button: 0, pointerId: 1 });
    await expect.poll(() => in1(page)).toBe(0);
  });

  test("holding 2 PLAYER sets IN1 bit 0x10, then releases", async ({ page }) => {
    const btn = page.locator("[data-start='2']");
    await btn.dispatchEvent("pointerdown", { button: 0, pointerId: 1 });
    await expect.poll(() => in1(page)).toBe(0x10);
    await btn.dispatchEvent("pointerup", { button: 0, pointerId: 1 });
    await expect.poll(() => in1(page)).toBe(0);
  });

  function pollsCoinBit(page: import("@playwright/test").Page, bit: number) {
    return page.evaluate((b) => {
      const m = (window as any).__test.machine;
      return new Promise<boolean>((resolve) => {
        const start = Date.now();
        const id = setInterval(() => {
          if ((m.in1() & b) !== 0) {
            clearInterval(id);
            resolve(true);
          } else if (Date.now() - start > 1500) {
            clearInterval(id);
            resolve(false);
          }
        }, 10);
      });
    }, bit);
  }

  test("clicking a 25¢ slot pulses IN1 bit 0x01, then releases", async ({ page }) => {
    expect(await in1(page)).toBe(0);
    const sawPulse = pollsCoinBit(page, 0x01);
    await page.locator("#coinDoor [data-coin]").first().click();
    expect(await sawPulse).toBe(true);
    await expect.poll(() => in1(page)).toBe(0);
  });

  test("the other slot is the same coin bit", async ({ page }) => {
    const sawPulse = pollsCoinBit(page, 0x01);
    await page.locator("#coinDoor [data-coin]").nth(1).click();
    expect(await sawPulse).toBe(true);
    await expect.poll(() => in1(page)).toBe(0);
  });
});
