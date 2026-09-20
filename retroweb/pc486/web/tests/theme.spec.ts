// EGA canvas; boot detection via window.__test.machine.textScreen() instead of terminal text
import { test, expect } from "./fixtures";
import { boot, waitForScreen } from "./helpers";

test.describe("page theme", () => {
  test.beforeEach(async ({ page }) => {
    await boot(page);
  });

  test("defaults to Windows 95 (retro8080.theme unset)", async ({ page }) => {
    await expect(page.locator("html")).toHaveAttribute("data-theme", "win");
    await expect(page.locator("#pageTheme")).toHaveValue("win");
  });

  test("every theme is selectable and sets the expected data-theme/data-mode", async ({ page }) => {
    const cases: [string, string, string | null][] = [
      ["win", "win", null],
      ["web94", "web94", null],
      ["modern", "modern", null],
      ["moderndark", "modern", "dark"],
    ];
    for (const [value, theme, mode] of cases) {
      await page.selectOption("#pageTheme", value);
      await expect(page.locator("html")).toHaveAttribute("data-theme", theme);
      const gotMode = await page.evaluate(() => document.documentElement.dataset.mode ?? null);
      expect(gotMode).toBe(mode);
    }
  });

  test("persists across reload via the shared retro8080.theme key", async ({ page }) => {
    await page.selectOption("#pageTheme", "web94");
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 15000 });
    await waitForScreen(page, /C:\\>/);
    await expect(page.locator("html")).toHaveAttribute("data-theme", "web94");
    await expect(page.locator("#pageTheme")).toHaveValue("web94");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe("web94");
  });
});
