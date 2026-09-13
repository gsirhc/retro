import { test, expect } from "@playwright/test";

// "Click to focus" banner (shared/focus-hint.js, same mechanism ibmpc-at's
// own keyboard.spec.ts test and altair8800's own focus-hint.spec.ts test
// cover) -- purely a web-UI convenience, no hardware equivalent, since a
// real board's keyboard is just whatever terminal is wired to the ACIA.

test.describe("focus hint", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/");
    await expect(page.locator("#screen .xterm-rows")).toContainText("\\", { timeout: 45000 });
  });

  test('"Click to focus" hint shows only while running and unfocused', async ({ page }) => {
    const hintVisible = () =>
      page.locator("#focusHint").evaluate((el) => el.classList.contains("visible"));
    // this page's own boot flow calls term.focus() right away (see app.js),
    // same as altair8800's, so the hint starts out hidden.
    expect(await hintVisible()).toBe(false);
    // clicking any other control blurs the terminal -- the hint returns
    await page.locator("#fullscreenBtn").focus();
    expect(await hintVisible()).toBe(true);
    await page.evaluate(() => (window as any).__term.focus());
    expect(await hintVisible()).toBe(false);
    // nothing to type into once powered off -- hidden regardless of focus
    await page.locator("#fullscreenBtn").focus();
    expect(await hintVisible()).toBe(true);
    await page.click('#pcbSvg [data-ref="J1"]');
    expect(await hintVisible()).toBe(false);
  });
});
