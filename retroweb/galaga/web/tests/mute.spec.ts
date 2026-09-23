import { test, expect } from "@playwright/test";

test.describe("Mute", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await page.waitForFunction(() => (window as any).__test);
  });

  test("is off by default and can be checked", async ({ page }) => {
    const box = page.locator("#mute");
    await expect(box).toBeVisible();
    await expect(box).not.toBeChecked();
    expect(await page.evaluate(() => (window as any).__test.muted)).toBe(false);

    await box.check();
    await expect(box).toBeChecked();
    expect(await page.evaluate(() => (window as any).__test.muted)).toBe(true);
  });

  test("hwtest ROM stays silent", async ({ page }) => {
    const heard = await page.evaluate(() => {
      const m = (window as any).__test.machine;
      m.runCycles(3072000 * 4);
      const s = m.drainAudio();
      for (let i = 0; i < s.length; i++) if (s[i] !== 0) return true;
      return false;
    });
    expect(heard).toBe(false);
  });
});
