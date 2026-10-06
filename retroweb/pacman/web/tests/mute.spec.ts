import { test, expect } from "@playwright/test";

test.describe("Mute", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await page.waitForFunction(() => (window as any).__test);
  });

  test("is off by default and can be toggled", async ({ page }) => {
    const btn = page.locator("#mute");
    await expect(btn).toBeVisible();
    await expect(btn).toHaveAttribute("aria-pressed", "false");
    expect(await page.evaluate(() => (window as any).__test.muted)).toBe(false);

    await btn.click();
    await expect(btn).toHaveAttribute("aria-pressed", "true");
    expect(await page.evaluate(() => (window as any).__test.muted)).toBe(true);
  });
});
