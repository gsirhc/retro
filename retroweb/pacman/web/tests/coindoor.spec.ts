import { test, expect } from "@playwright/test";

// Photoreal 25¢ lamps + coin slots (index.html's #coinDoor). A labelled
// web-UI stand-in for dropping a quarter: clicking a slot pulses the
// same active-low IN0 bit 0x20 as the 5 key. Not a model of the cabinet's
// coin-counter / lockout solenoids.

test.describe("coin door", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await expect(page.locator("#coinDoor")).toBeVisible();
    await expect.poll(() => page.evaluate(() => !!(window as any).__test)).toBe(true);
  });

  const in0 = (page: import("@playwright/test").Page) =>
    page.evaluate(() => (window as any).__test.machine.in0());

  test("shows two 25¢ lamps and coin slots on the yellow-bordered plate", async ({ page }) => {
    await expect(page.locator(".cab-face")).toBeVisible();
    await expect(page.locator("#coinDoor [data-coin]")).toHaveCount(2);
    await expect(page.locator(".cd-lamp")).toHaveCount(2);
    await expect(page.locator(".cd-lamp").first()).toHaveText("25¢");
    const box = await page.locator(".cd-lamp").first().boundingBox();
    expect(box!.width).toBeGreaterThan(60);
  });

  test("shows 1 PLAYER and 2 PLAYER start buttons above the slots", async ({ page }) => {
    await expect(page.locator("[data-start]")).toHaveCount(2);
    await expect(page.locator("[data-start='1']")).toHaveAttribute("aria-label", "1 Player start");
    await expect(page.locator("[data-start='2']")).toHaveAttribute("aria-label", "2 Player start");
    await expect(page.locator("[data-start='1'] .cab-start-img")).toBeVisible();
    await expect(page.locator("[data-start='2'] .cab-start-img")).toBeVisible();
  });

  const in1 = (page: import("@playwright/test").Page) =>
    page.evaluate(() => (window as any).__test.machine.in1());

  test("holding 1 PLAYER clears IN1 bit 0x20, then releases", async ({ page }) => {
    expect(await in1(page)).toBe(0xFF);
    const btn = page.locator("[data-start='1']");
    await btn.dispatchEvent("pointerdown", { button: 0, pointerId: 1 });
    await expect.poll(() => in1(page)).toBe(0xFF & ~0x20);
    await btn.dispatchEvent("pointerup", { button: 0, pointerId: 1 });
    await expect.poll(() => in1(page)).toBe(0xFF);
  });

  test("holding 2 PLAYER clears IN1 bit 0x40, then releases", async ({ page }) => {
    const btn = page.locator("[data-start='2']");
    await btn.dispatchEvent("pointerdown", { button: 0, pointerId: 1 });
    await expect.poll(() => in1(page)).toBe(0xFF & ~0x40);
    await btn.dispatchEvent("pointerup", { button: 0, pointerId: 1 });
    await expect.poll(() => in1(page)).toBe(0xFF);
  });

  test("clicking a 25¢ slot pulses IN0 coin (bit 0x20), then releases", async ({ page }) => {
    expect(await in0(page)).toBe(0xFF);

    const sawPulse = page.evaluate(() => {
      const m = (window as any).__test.machine;
      return new Promise<boolean>((resolve) => {
        const start = Date.now();
        const id = setInterval(() => {
          if ((m.in0() & 0x20) === 0) {
            clearInterval(id);
            resolve(true);
          } else if (Date.now() - start > 1500) {
            clearInterval(id);
            resolve(false);
          }
        }, 10);
      });
    });
    await page.locator("#coinDoor [data-coin]").first().click();
    expect(await sawPulse).toBe(true);
    await expect.poll(() => in0(page)).toBe(0xFF);
  });

  test("the other slot is the same coin bit, not a second mech", async ({ page }) => {
    const sawPulse = page.evaluate(() => {
      const m = (window as any).__test.machine;
      return new Promise<boolean>((resolve) => {
        const start = Date.now();
        const id = setInterval(() => {
          if ((m.in0() & 0x20) === 0) {
            clearInterval(id);
            resolve(true);
          } else if (Date.now() - start > 1500) {
            clearInterval(id);
            resolve(false);
          }
        }, 10);
      });
    });
    await page.locator("#coinDoor [data-coin]").nth(1).click();
    expect(await sawPulse).toBe(true);
  });
});
