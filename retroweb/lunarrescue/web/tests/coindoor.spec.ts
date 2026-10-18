import { test, expect, type Page } from "@playwright/test";

const in1 = (page: Page) => page.evaluate(() => (window as any).__test.machine.in1());

// Watches IN1 for the active-low COIN pulse.
const watchCoin = (page: Page) => page.evaluate(() => {
  const m = (window as any).__test.machine;
  return new Promise<boolean>((resolve) => {
    const start = Date.now();
    const id = setInterval(() => {
      if ((m.in1() & 0x01) === 0) {
        clearInterval(id);
        resolve(true);
      } else if (Date.now() - start > 1500) {
        clearInterval(id);
        resolve(false);
      }
    }, 10);
  });
});

test.describe("coin door", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await expect(page.locator("#coinDoor")).toBeVisible();
    await expect.poll(() => page.evaluate(() => !!(window as any).__test)).toBe(true);
  });

  test("shows two 25¢ lamps and coin slots on the charcoal plate", async ({ page }) => {
    const face = page.locator(".cab-face");
    await expect(face).toBeVisible();
    const bg = await face.evaluate((el) => getComputedStyle(el).backgroundImage);
    expect(bg).toMatch(/52,\s*52,\s*58|34,\s*34,\s*38|18,\s*18,\s*20/);
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

  test("holding 1 PLAYER sets IN1 bit 0x04, then releases", async ({ page }) => {
    expect((await in1(page)) & 0x04).toBe(0);
    const btn = page.locator("[data-start='1']");
    await btn.dispatchEvent("pointerdown", { button: 0, pointerId: 1 });
    await expect.poll(async () => (await in1(page)) & 0x04).toBe(0x04);
    await btn.dispatchEvent("pointerup", { button: 0, pointerId: 1 });
    await expect.poll(async () => (await in1(page)) & 0x04).toBe(0);
  });

  test("holding 2 PLAYER sets IN1 bit 0x02, then releases", async ({ page }) => {
    const btn = page.locator("[data-start='2']");
    await btn.dispatchEvent("pointerdown", { button: 0, pointerId: 1 });
    await expect.poll(async () => (await in1(page)) & 0x02).toBe(0x02);
    await btn.dispatchEvent("pointerup", { button: 0, pointerId: 1 });
    await expect.poll(async () => (await in1(page)) & 0x02).toBe(0);
  });

  test("clicking a 25¢ slot pulses COIN low, then releases", async ({ page }) => {
    expect(await in1(page)).toBe(0x89);
    const sawPulse = watchCoin(page);
    await page.locator("#coinDoor [data-coin]").first().click();
    expect(await sawPulse).toBe(true);
    await expect.poll(() => in1(page)).toBe(0x89);
  });

  test("the other slot is the same coin line, not a second mech", async ({ page }) => {
    const sawPulse = watchCoin(page);
    await page.locator("#coinDoor [data-coin]").nth(1).click();
    expect(await sawPulse).toBe(true);
  });
});
