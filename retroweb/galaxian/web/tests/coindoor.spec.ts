import { test, expect } from "@playwright/test";

test.describe("coin door", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await expect(page.locator("#coinDoor")).toBeVisible();
    await expect.poll(() => page.evaluate(() => !!(window as any).__test)).toBe(true);
  });

  const in0 = (page: import("@playwright/test").Page) =>
    page.evaluate(() => (window as any).__test.machine.in0());

  test("shows two 25¢ lamps and coin slots on the navy-bordered plate", async ({ page }) => {
    const face = page.locator(".cab-face");
    await expect(face).toBeVisible();
    const bg = await face.evaluate((el) => getComputedStyle(el).backgroundImage);
    expect(bg).not.toMatch(/243,\s*225,\s*74/);  // Pac-Man yellow
    expect(bg).toMatch(/42,\s*58,\s*138|26,\s*40,\s*104|14,\s*24,\s*72/);
    await expect(page.locator("#coinDoor [data-coin]")).toHaveCount(2);
    await expect(page.locator(".cd-lamp")).toHaveCount(2);
    await expect(page.locator(".cd-lamp").first()).toHaveText("25¢");
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

  test("holding 1 PLAYER sets IN1 bit 0x01, then releases", async ({ page }) => {
    const idle = await in1(page);
    expect(idle & 0x01).toBe(0);
    const btn = page.locator("[data-start='1']");
    await btn.dispatchEvent("pointerdown", { button: 0, pointerId: 1 });
    await expect.poll(async () => (await in1(page)) & 0x01).toBe(0x01);
    await btn.dispatchEvent("pointerup", { button: 0, pointerId: 1 });
    await expect.poll(async () => (await in1(page)) & 0x01).toBe(0);
  });

  test("holding 2 PLAYER sets IN1 bit 0x02, then releases", async ({ page }) => {
    const btn = page.locator("[data-start='2']");
    await btn.dispatchEvent("pointerdown", { button: 0, pointerId: 1 });
    await expect.poll(async () => (await in1(page)) & 0x02).toBe(0x02);
    await btn.dispatchEvent("pointerup", { button: 0, pointerId: 1 });
    await expect.poll(async () => (await in1(page)) & 0x02).toBe(0);
  });

  test("clicking a 25¢ slot pulses IN0 coin (bit 0x01), then releases", async ({ page }) => {
    expect(await in0(page)).toBe(0x00);
    const sawPulse = page.evaluate(() => {
      const m = (window as any).__test.machine;
      return new Promise<boolean>((resolve) => {
        const start = Date.now();
        const id = setInterval(() => {
          if ((m.in0() & 0x01) !== 0) {
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
    await expect.poll(() => in0(page)).toBe(0x00);
  });

  test("the other slot is the same coin bit, not a second mech", async ({ page }) => {
    const sawPulse = page.evaluate(() => {
      const m = (window as any).__test.machine;
      return new Promise<boolean>((resolve) => {
        const start = Date.now();
        const id = setInterval(() => {
          if ((m.in0() & 0x01) !== 0) {
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
