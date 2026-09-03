import { test, expect } from "./fixtures";
import { boot, waitForScreen } from "./helpers";

test.describe("page chrome, persistence, URL params", () => {
  test("the page theme select drives data-theme and persists", async ({ page }) => {
    await boot(page);
    for (const t of ["web94", "modern", "win"]) {
      await page.selectOption("#pageTheme", t);
      await expect(page.locator("html")).toHaveAttribute("data-theme", t);
      expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe(t);
    }
  });

  test("?theme= wins at load", async ({ page }) => {
    await boot(page, { params: "theme=web94" });
    await expect(page.locator("html")).toHaveAttribute("data-theme", "web94");
  });

  test("a stored theme is restored on the next visit", async ({ page }) => {
    await boot(page);
    await page.selectOption("#pageTheme", "modern");
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine);
    await expect(page.locator("html")).toHaveAttribute("data-theme", "modern");
  });

  test("the register bar shows A / BC / DE / HL / PC / SP and a clock rate", async ({ page }) => {
    await boot(page);
    await expect
      .poll(() => page.locator("#regs").innerText())
      .toMatch(/A [0-9A-F]{2}\s+BC [0-9A-F]{4}\s+DE [0-9A-F]{4}\s+HL [0-9A-F]{4}\s+PC [0-9A-F]{4}\s+SP [0-9A-F]{4}/);
    await expect.poll(() => page.locator("#regs").innerText()).toMatch(/\d\.\d MHz/);
  });

  test("the register bar reads STOP after a panel STOP", async ({ page }) => {
    await boot(page);
    await page.evaluate(() => (window as any).__test.setRunning(false));
    await expect.poll(() => page.locator("#regs").innerText()).toMatch(/STOP/);
  });

  test("?help prints the manual to the terminal", async ({ page }) => {
    await boot(page, { params: "help" });
    await waitForScreen(page, /front panel|ALTAIR|terminal/i, 15_000);
  });

  test("a keypress skips to the end of the slow ?help printout", async ({ page }) => {
    await boot(page, { params: "help&term=tty33" }); // metered at 110 baud
    await page.waitForTimeout(500);
    await page.evaluate(() => (window as any).__test.term.focus());
    await page.keyboard.press("Space");
    // the whole manual lands at once instead of trickling for ~30 s
    await waitForScreen(page, /RESET|CLR|SINGLE STEP/i, 5_000);
  });

  test("the build date populates from the wasm's Last-Modified", async ({ page }) => {
    await boot(page);
    await expect.poll(() => page.locator("#buildDate").innerText()).not.toBe("");
    await expect(page.locator("#buildDate")).not.toHaveText(/unknown/);
  });

  test("load devices persist across a reload in a custom build", async ({ page }) => {
    await boot(page);
    await page.selectOption("#preset", "");
    await expect(page.locator("#deviceChips")).toBeVisible();
    await page.check('#deviceChips input[data-dev="disk"]');
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine);
    await expect(page.locator('#deviceChips input[data-dev="disk"]')).toBeChecked();
    await expect(page.locator("#dcdd")).not.toHaveClass(/empty/);
  });
});
