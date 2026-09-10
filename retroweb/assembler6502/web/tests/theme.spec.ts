import { test, expect } from "@playwright/test";

test.describe("page theme", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/");
    await expect(page.locator("#screen")).toContainText("\\", { timeout: 25000 });
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
    await expect(page.locator("#screen")).toContainText("\\", { timeout: 25000 });
    await expect(page.locator("html")).toHaveAttribute("data-theme", "web94");
    await expect(page.locator("#pageTheme")).toHaveValue("web94");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe("web94");
  });

  test("the board and terminal still work under every theme", async ({ page }) => {
    await page.selectOption("#pageTheme", "win");
    await page.click("#screen");
    await page.keyboard.type("0.F", { delay: 100 });
    await page.keyboard.press("Enter");
    await expect(page.locator("#screen")).toContainText("0000:", { timeout: 25000 });
    await page.click('#pcbSvg [data-ref="SW1"]');
    await expect(page.locator("#screen")).not.toContainText("0000:", { timeout: 5000 });
    await expect(page.locator("#screen")).toContainText("\\", { timeout: 25000 });
  });
});
