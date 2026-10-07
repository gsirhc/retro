// EGA canvas; boot detection via window.__test.machine.textScreen()
import { type Page } from "@playwright/test";
import { test, expect } from "./fixtures";
import { bootLive } from "./helpers";

async function chooseTheme(page: Page, theme: string, mode = "light") {
  await page.locator("#pageThemeBtn").click();
  await page.locator(`#themeDialog input[name="pageThemeFamily"][value="${theme}"]`).check();
  await page.locator(`#themeDialog input[name="pageThemeMode"][value="${mode}"]`).check();
  await page.locator("#themeDialogDone").click();
}

test.describe("page theme", () => {
  test("defaults to Windows 95 (retro8080.theme unset)", async ({ livePage: page }) => {
    await expect(page.locator("html")).toHaveAttribute("data-theme", "win");
    await expect(page.locator("#pageThemeBtn")).toHaveText("Windows 95");
  });

  test("every theme is selectable and sets the expected data-theme/data-mode", async ({ livePage: page }) => {
    const cases: [string, string, string, string | null][] = [
      ["win", "light", "win", null],
      ["web94", "light", "web94", null],
      ["modern", "light", "modern", null],
      ["modern", "dark", "modern", "dark"],
    ];
    for (const [theme, mode, dataTheme, dataMode] of cases) {
      await chooseTheme(page, theme, mode);
      await expect(page.locator("html")).toHaveAttribute("data-theme", dataTheme);
      const gotMode = await page.evaluate(() => document.documentElement.dataset.mode ?? null);
      expect(gotMode).toBe(dataMode);
    }
  });

  test("System mode follows the OS appearance on the Modern palette", async ({ livePage: page }) => {
    try {
      await page.emulateMedia({ colorScheme: "light" });
      await chooseTheme(page, "modern", "system");
      await expect(page.locator("html")).toHaveAttribute("data-theme", "modern");
      await expect(page.locator("html")).not.toHaveAttribute("data-mode");
      expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe("modern");
      expect(await page.evaluate(() => localStorage.getItem("retro8080.mode"))).toBe("system");

      await page.emulateMedia({ colorScheme: "dark" });
      await expect(page.locator("html")).toHaveAttribute("data-theme", "modern");
      await expect(page.locator("html")).toHaveAttribute("data-mode", "dark");

      await chooseTheme(page, "modern", "light");
      await page.emulateMedia({ colorScheme: "dark" });
      await expect(page.locator("html")).toHaveAttribute("data-theme", "modern");
      await expect(page.locator("html")).not.toHaveAttribute("data-mode");
    } finally {
      await page.emulateMedia({ colorScheme: "light" });
    }
  });

  test("System mode follows the OS appearance on the Aurora palette", async ({ livePage: page }) => {
    try {
      await page.emulateMedia({ colorScheme: "light" });
      await chooseTheme(page, "aurora", "system");
      await expect(page.locator("html")).toHaveAttribute("data-theme", "aurora");
      await expect(page.locator("html")).not.toHaveAttribute("data-mode");
      expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe("aurora");
      expect(await page.evaluate(() => localStorage.getItem("retro8080.mode"))).toBe("system");

      await page.emulateMedia({ colorScheme: "dark" });
      await expect(page.locator("html")).toHaveAttribute("data-theme", "aurora");
      await expect(page.locator("html")).toHaveAttribute("data-mode", "dark");
    } finally {
      await page.emulateMedia({ colorScheme: "light" });
    }
  });

  test("persists across reload via the shared theme and mode keys", async ({ page }) => {
    await bootLive(page);
    await chooseTheme(page, "web94");
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 15000 });
    await expect(page.locator("html")).toHaveAttribute("data-theme", "web94");
    await expect(page.locator("#pageThemeBtn")).toHaveText("Mid-1990s Web");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe("web94");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.mode"))).toBe("light");
  });
});
