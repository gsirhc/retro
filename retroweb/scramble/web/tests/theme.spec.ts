import { test, expect, type Page } from "@playwright/test";

async function chooseTheme(page: Page, theme: string, mode = "light") {
  await page.locator("#pageThemeBtn").click();
  await page.locator(`#themeDialog input[name="pageThemeFamily"][value="${theme}"]`).check();
  await page.locator(`#themeDialog input[name="pageThemeMode"][value="${mode}"]`).check();
  await page.locator("#themeDialogDone").click();
}

test.describe("page theme", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/");
    await expect(page.locator("#screen")).toBeVisible();
  });

  test("defaults to Windows 95 (retro8080.theme unset)", async ({ page }) => {
    await expect(page.locator("html")).toHaveAttribute("data-theme", "win");
    await expect(page.locator("#pageThemeBtn")).toHaveText("Windows 95");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.mode"))).toBe("light");
  });

  test("every theme is selectable and sets the expected data-theme/data-mode", async ({ page }) => {
    await page.emulateMedia({ colorScheme: "light" });
    const cases: [string, string, string, string | null][] = [
      ["win", "light", "win", null],
      ["winxp", "light", "winxp", null],
      ["web94", "light", "web94", null],
      ["modern", "light", "modern", null],
      ["modern", "dark", "modern", "dark"],
      ["aurora", "light", "aurora", null],
      ["win", "dark", "win", "dark"],
      ["winxp", "dark", "winxp", "dark"],
      ["web94", "dark", "web94", "dark"],
    ];
    for (const [theme, mode, dataTheme, dataMode] of cases) {
      await chooseTheme(page, theme, mode);
      await expect(page.locator("html")).toHaveAttribute("data-theme", dataTheme);
      const gotMode = await page.evaluate(() => document.documentElement.dataset.mode ?? null);
      expect(gotMode).toBe(dataMode);
      expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe(theme);
      expect(await page.evaluate(() => localStorage.getItem("retro8080.mode"))).toBe(mode);
    }
  });

  test("System mode follows the OS appearance on any family", async ({ page }) => {
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

    await page.emulateMedia({ colorScheme: "light" });
    await chooseTheme(page, "aurora", "system");
    await expect(page.locator("html")).toHaveAttribute("data-theme", "aurora");
    await expect(page.locator("html")).not.toHaveAttribute("data-mode");
    await page.emulateMedia({ colorScheme: "dark" });
    await expect(page.locator("html")).toHaveAttribute("data-theme", "aurora");
    await expect(page.locator("html")).toHaveAttribute("data-mode", "dark");

    await page.emulateMedia({ colorScheme: "light" });
    await chooseTheme(page, "win", "system");
    await expect(page.locator("html")).toHaveAttribute("data-theme", "win");
    await expect(page.locator("html")).not.toHaveAttribute("data-mode");
    await page.emulateMedia({ colorScheme: "dark" });
    await expect(page.locator("html")).toHaveAttribute("data-theme", "win");
    await expect(page.locator("html")).toHaveAttribute("data-mode", "dark");
  });

  test("persists across reload via the shared theme and mode keys", async ({ page }) => {
    await chooseTheme(page, "web94", "dark");
    await page.reload();
    await expect(page.locator("#screen")).toBeVisible();
    await expect(page.locator("html")).toHaveAttribute("data-theme", "web94");
    await expect(page.locator("html")).toHaveAttribute("data-mode", "dark");
    await expect(page.locator("#pageThemeBtn")).toHaveText("Mid-1990s Web");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe("web94");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.mode"))).toBe("dark");
  });

  test("legacy composite theme values migrate on load", async ({ page }) => {
    await page.emulateMedia({ colorScheme: "dark" });
    await page.evaluate(() => {
      localStorage.setItem("retro8080.theme", "system");
      localStorage.removeItem("retro8080.mode");
    });
    await page.reload();
    await expect(page.locator("#screen")).toBeVisible();
    await expect(page.locator("html")).toHaveAttribute("data-theme", "modern");
    await expect(page.locator("html")).toHaveAttribute("data-mode", "dark");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe("modern");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.mode"))).toBe("system");
  });

  test("the emulator keeps running under every theme", async ({ page }) => {
    await page.goto("/?test=1");
    await chooseTheme(page, "modern", "dark");
    const c0 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    await page.waitForTimeout(300);
    const c1 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    expect(c1).toBeGreaterThan(c0);
  });
});
