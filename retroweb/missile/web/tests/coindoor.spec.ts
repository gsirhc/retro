import { test, expect } from "@playwright/test";

// Left slot pulses the left coin (IN0 bit 5), right slot the right coin (bit 7), same as keys 5 and 6.
// IN0 is active-low. The center coin mech is unmapped.

test.describe("coin door", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await expect(page.locator("#coinDoor")).toBeVisible();
    await expect.poll(() => page.evaluate(() => !!(window as any).__test)).toBe(true);
  });

  const in0 = (page: import("@playwright/test").Page) =>
    page.evaluate(() => (window as any).__test.machine.in0());

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

  test("holding 1 PLAYER clears IN0 bit 0x10, then releases", async ({ page }) => {
    expect(await in0(page)).toBe(0xff);
    const btn = page.locator("[data-start='1']");
    await btn.dispatchEvent("pointerdown", { button: 0, pointerId: 1 });
    await expect.poll(() => in0(page)).toBe(0xef);
    await btn.dispatchEvent("pointerup", { button: 0, pointerId: 1 });
    await expect.poll(() => in0(page)).toBe(0xff);
  });

  test("holding 2 PLAYER clears IN0 bit 0x08, then releases", async ({ page }) => {
    const btn = page.locator("[data-start='2']");
    await btn.dispatchEvent("pointerdown", { button: 0, pointerId: 1 });
    await expect.poll(() => in0(page)).toBe(0xf7);
    await btn.dispatchEvent("pointerup", { button: 0, pointerId: 1 });
    await expect.poll(() => in0(page)).toBe(0xff);
  });

  function pollsCoinBit(page: import("@playwright/test").Page, bit: number) {
    return page.evaluate((b) => {
      const m = (window as any).__test.machine;
      return new Promise<boolean>((resolve) => {
        const start = Date.now();
        const id = setInterval(() => {
          if ((m.in0() & b) === 0) {
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

  test("clicking the left 25¢ slot pulses the left coin (IN0 bit 0x20 low), then releases", async ({ page }) => {
    expect(await in0(page)).toBe(0xff);
    const sawPulse = pollsCoinBit(page, 0x20);
    await page.locator("#coinDoor [data-coin]").first().click();
    expect(await sawPulse).toBe(true);
    await expect.poll(() => in0(page)).toBe(0xff);
  });

  test("clicking the right 25¢ slot pulses the right coin (IN0 bit 0x80 low), then releases", async ({ page }) => {
    const sawPulse = pollsCoinBit(page, 0x80);
    await page.locator("#coinDoor [data-coin]").nth(1).click();
    expect(await sawPulse).toBe(true);
    await expect.poll(() => in0(page)).toBe(0xff);
  });
});
