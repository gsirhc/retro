import { test, expect } from "@playwright/test";

// Fullscreen expands #bezel. No Esc hint dialog here, keys are not serial input.

test.describe("fullscreen", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/");
    await expect(page.locator("#screen")).toBeVisible();
  });

  test("clicking Fullscreen enters fullscreen on the bezel immediately (no hint dialog)", async ({ page }) => {
    await page.locator("#fullscreenBtn").click();
    await expect.poll(() => page.evaluate(() => !!document.fullscreenElement)).toBe(true);
    expect(await page.evaluate(() => document.fullscreenElement?.id)).toBe("bezel");
    await expect(page.locator("#fullscreenBtn")).toHaveAttribute("aria-label", "Exit fullscreen");
  });

  test("clicking it again exits fullscreen and resets the button label", async ({ page }) => {
    await page.locator("#fullscreenBtn").click();
    await expect.poll(() => page.evaluate(() => !!document.fullscreenElement)).toBe(true);
    await page.locator("#fullscreenBtn").click();
    await expect.poll(() => page.evaluate(() => !!document.fullscreenElement)).toBe(false);
    await expect(page.locator("#fullscreenBtn")).toHaveAttribute("aria-label", "Fullscreen");
  });

  test("entering fullscreen scales the canvas up, keeping its aspect ratio", async ({ page }) => {
    await page.setViewportSize({ width: 1600, height: 900 });
    const before = await page.locator("#screen").boundingBox();
    await page.locator("#fullscreenBtn").click();
    await expect.poll(() => page.evaluate(() => !!document.fullscreenElement)).toBe(true);
    await expect
      .poll(() => page.locator("#screen").boundingBox().then((b) => b!.height))
      .toBeGreaterThan(before!.height * 1.2);
    const after = await page.locator("#screen").boundingBox();
    const bezel = await page.locator("#bezel").boundingBox();
    // 224x288 aspect ratio preserved (within a pixel of rounding).
    expect(Math.abs(after!.width / after!.height - before!.width / before!.height)).toBeLessThan(0.02);
    expect(Math.abs((after!.x + after!.width / 2) - (bezel!.x + bezel!.width / 2))).toBeLessThan(4);
    expect(Math.abs((after!.y + after!.height / 2) - (bezel!.y + bezel!.height / 2))).toBeLessThan(8);
    expect(bezel!.height).toBeGreaterThan(900 * 0.98);
    expect(after!.height).toBeGreaterThan(bezel!.height * 0.98);
  });

  test("the machine keeps running while fullscreen", async ({ page }) => {
    await page.goto("/?test=1");
    await page.locator("#fullscreenBtn").click();
    await expect.poll(() => page.evaluate(() => !!document.fullscreenElement)).toBe(true);
    const c0 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    await page.waitForTimeout(300);
    const c1 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    expect(c1).toBeGreaterThan(c0);
  });
});
