import { test, expect, type Page } from "@playwright/test";

// The left slot pulses IN0 bit 0x02 low (same as 5), the right slot bit 0x01 (same as 6).

test.describe("coin door", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await expect(page.locator("#coinDoor")).toBeVisible();
    await expect.poll(() => page.evaluate(() => !!(window as any).__test?.machine)).toBe(true);
  });

  const in0 = (page: Page) => page.evaluate(() => (window as any).__test.machine.in0());
  const in3 = (page: Page) => page.evaluate(() => (window as any).__test.machine.in3());

  test("shows two 25¢ slots with the mute control between them", async ({ page }) => {
    await expect(page.locator(".cab-face")).toBeVisible();
    await expect(page.locator("#coinDoor [data-coin]")).toHaveCount(2);
    await expect(page.locator("#coinDoor .cd-mech[data-coin='left']")).toBeVisible();
    await expect(page.locator("#coinDoor .cd-mech[data-coin='right']")).toBeVisible();
    await expect(page.locator(".cd-lamp")).toHaveCount(2);
    await expect(page.locator(".cd-lamp").first()).toHaveText("25¢");
    await expect(page.locator("#mute")).toBeVisible();
    const order = await page.locator("#coinDoor > *").evaluateAll((els) => els.map((e) => e.id || e.getAttribute("data-coin")));
    expect(order).toEqual(["left", "mute", "right"]);
  });

  test("has a single START button, no second player", async ({ page }) => {
    await expect(page.locator("[data-start]")).toHaveCount(1);
    await expect(page.locator("[data-start='1']")).toHaveAttribute("aria-label", "Start");
    await expect(page.locator("[data-start='2']")).toHaveCount(0);
    await expect(page.locator("[data-start='1'] .cab-start-img")).toBeVisible();
  });

  test("holding START sets IN3 bit 0x20, then releases", async ({ page }) => {
    expect(await in3(page)).toBe(0);
    const btn = page.locator("[data-start='1']");
    await btn.dispatchEvent("pointerdown", { button: 0, pointerId: 1 });
    await expect.poll(() => in3(page)).toBe(0x20);
    await expect(btn).toHaveClass(/pressed/);
    await btn.dispatchEvent("pointerup", { button: 0, pointerId: 1 });
    await expect.poll(() => in3(page)).toBe(0);
    await expect(btn).not.toHaveClass(/pressed/);
  });

  test("the start lamp follows the sound latch bit 0x40", async ({ page }) => {
    const btn = page.locator("[data-start='1']");
    await page.waitForFunction(() => (window as any).__test.machine.frames() > 30);
    await page.evaluate(() => (window as any).__test.machine.setRamByte(0x1840, 0x60));
    await expect(btn).toHaveClass(/lamp-on/);
    await page.evaluate(() => (window as any).__test.machine.setRamByte(0x1840, 0x20));
    await expect(btn).not.toHaveClass(/lamp-on/);
  });

  // Records the lowest IN0 seen over the next frames, resolving once the bit drops or time runs out.
  function sawLow(page: Page, bit: number) {
    return page.evaluate((b) => {
      const m = (window as any).__test.machine;
      return new Promise<boolean>((resolve) => {
        const end = performance.now() + 1500;
        const step = () => {
          if ((m.in0() & b) === 0) resolve(true);
          else if (performance.now() > end) resolve(false);
          else requestAnimationFrame(step);
        };
        step();
      });
    }, bit);
  }

  for (const [slot, bit] of [["left", 0x02], ["right", 0x01]] as const) {
    test(`clicking the ${slot} slot pulses IN0 bit 0x${bit.toString(16)} low, then releases`, async ({ page }) => {
      expect(await in0(page)).toBe(0x3f);
      await page.evaluate(() => {
        const door = document.getElementById("coinDoor")!;
        (window as any).__coined = door.classList.contains("coined");
        new MutationObserver(() => { if (door.classList.contains("coined")) (window as any).__coined = true; })
          .observe(door, { attributes: true, attributeFilter: ["class"] });
      });
      const saw = sawLow(page, bit);
      await page.locator(`#coinDoor [data-coin='${slot}']`).click();
      expect(await saw).toBe(true);
      await page.waitForFunction(() => (window as any).__coined === true);
      await expect.poll(() => in0(page)).toBe(0x3f);
      await expect(page.locator("#coinDoor")).not.toHaveClass(/coined/);
    });
  }
});
