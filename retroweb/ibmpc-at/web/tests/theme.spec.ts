// EGA canvas; boot detection via window.__test.machine.textScreen() instead of terminal text
import { type Page } from "@playwright/test";
import { test, expect } from "./fixtures";
import { boot, waitForScreen } from "./helpers";

async function chooseTheme(page: Page, theme: string, mode = "light") {
  await page.locator("#pageThemeBtn").click();
  await page.locator(`#themeDialog input[name="pageThemeFamily"][value="${theme}"]`).check();
  await page.locator(`#themeDialog input[name="pageThemeMode"][value="${mode}"]`).check();
  await page.locator("#themeDialogDone").click();
}

test.describe("page theme", () => {
  test.beforeEach(async ({ page }) => {
    await boot(page);
  });

  test("defaults to Windows 95 (retro8080.theme unset)", async ({ page }) => {
    await expect(page.locator("html")).toHaveAttribute("data-theme", "win");
    await expect(page.locator("#pageThemeBtn")).toHaveText("Windows 95");
  });

  test("every theme is selectable and sets the expected data-theme/data-mode", async ({ page }) => {
    const cases: [string, string, string, string | null][] = [
      ["win", "light", "win", null],
      ["winxp", "light", "winxp", null],
      ["web94", "light", "web94", null],
      ["modern", "light", "modern", null],
      ["modern", "dark", "modern", "dark"],
      ["aurora", "light", "aurora", null],
      ["winxp", "dark", "winxp", "dark"],
    ];
    for (const [theme, mode, dataTheme, dataMode] of cases) {
      await chooseTheme(page, theme, mode);
      await expect(page.locator("html")).toHaveAttribute("data-theme", dataTheme);
      const gotMode = await page.evaluate(() => document.documentElement.dataset.mode ?? null);
      expect(gotMode).toBe(dataMode);
    }
  });

  test("System mode follows the OS appearance on the Modern palette", async ({ page }) => {
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
  });

  test("System mode follows the OS appearance on the Aurora palette", async ({ page }) => {
    await page.emulateMedia({ colorScheme: "light" });
    await chooseTheme(page, "aurora", "system");
    await expect(page.locator("html")).toHaveAttribute("data-theme", "aurora");
    await expect(page.locator("html")).not.toHaveAttribute("data-mode");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe("aurora");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.mode"))).toBe("system");

    await page.emulateMedia({ colorScheme: "dark" });
    await expect(page.locator("html")).toHaveAttribute("data-theme", "aurora");
    await expect(page.locator("html")).toHaveAttribute("data-mode", "dark");
  });

  test("aurora and modern keep the case at the Win95 fascia width", async ({ page }) => {
    const vp = page.viewportSize()!;
    try {
      await page.setViewportSize({ width: 1400, height: 900 });
      await chooseTheme(page, "win");
      const winWidth = await page.evaluate(() =>
        document.querySelector("#frontPanelCard .at-case")!.getBoundingClientRect().width);
      for (const theme of ["aurora", "modern"] as const) {
        await chooseTheme(page, theme);
        const width = await page.evaluate(() =>
          document.querySelector("#frontPanelCard .at-case")!.getBoundingClientRect().width);
        expect(width).toBeLessThanOrEqual(winWidth + 1);
      }
    } finally {
      await page.setViewportSize(vp);
    }
  });

  test("persists across reload via the shared theme and mode keys", async ({ page }) => {
    await chooseTheme(page, "web94");
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 15000 });
    await waitForScreen(page, /C:\\>/);
    await expect(page.locator("html")).toHaveAttribute("data-theme", "web94");
    await expect(page.locator("#pageThemeBtn")).toHaveText("Mid-1990s Web");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe("web94");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.mode"))).toBe("light");
  });
});
