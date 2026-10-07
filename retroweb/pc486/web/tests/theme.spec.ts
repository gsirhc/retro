// EGA canvas; boot detection via window.__test.machine.textScreen() instead of terminal text
import { test, expect } from "./fixtures";
import { bootLive } from "./helpers";

test.describe("page theme", () => {
  test("defaults to Windows 95 (retro8080.theme unset)", async ({ livePage: page }) => {
    await expect(page.locator("html")).toHaveAttribute("data-theme", "win");
    await expect(page.locator("#pageTheme")).toHaveValue("win");
  });

  test("every theme is selectable and sets the expected data-theme/data-mode", async ({ livePage: page }) => {
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

  test("System follows the OS appearance on the Modern palette", async ({ livePage: page }) => {
    try {
      await page.emulateMedia({ colorScheme: "light" });
      await page.selectOption("#pageTheme", "system");
      await expect(page.locator("html")).toHaveAttribute("data-theme", "modern");
      await expect(page.locator("html")).not.toHaveAttribute("data-mode");
      expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe("system");

      await page.emulateMedia({ colorScheme: "dark" });
      await expect(page.locator("html")).toHaveAttribute("data-theme", "modern");
      await expect(page.locator("html")).toHaveAttribute("data-mode", "dark");
      await expect(page.locator("#pageTheme")).toHaveValue("system");

      // An explicit Modern choice stays light when the OS goes dark.
      await page.selectOption("#pageTheme", "modern");
      await page.emulateMedia({ colorScheme: "dark" });
      await expect(page.locator("html")).toHaveAttribute("data-theme", "modern");
      await expect(page.locator("html")).not.toHaveAttribute("data-mode");
    } finally {
      await page.emulateMedia({ colorScheme: "light" });
    }
  });

  test("System Aurora follows the OS appearance on the Aurora palette", async ({ livePage: page }) => {
    try {
      await page.emulateMedia({ colorScheme: "light" });
      await page.selectOption("#pageTheme", "systemaurora");
      await expect(page.locator("html")).toHaveAttribute("data-theme", "aurora");
      await expect(page.locator("html")).not.toHaveAttribute("data-mode");
      expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe("systemaurora");

      await page.emulateMedia({ colorScheme: "dark" });
      await expect(page.locator("html")).toHaveAttribute("data-theme", "aurora");
      await expect(page.locator("html")).toHaveAttribute("data-mode", "dark");
      await expect(page.locator("#pageTheme")).toHaveValue("systemaurora");
    } finally {
      await page.emulateMedia({ colorScheme: "light" });
    }
  });

  test("persists across reload via the shared retro8080.theme key", async ({ page }) => {
    await bootLive(page);
    await page.selectOption("#pageTheme", "web94");
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 15000 });
    await expect(page.locator("html")).toHaveAttribute("data-theme", "web94");
    await expect(page.locator("#pageTheme")).toHaveValue("web94");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe("web94");
  });
});
