import { test, expect } from "./fixtures";
import { boot } from "./helpers";

// "Click to focus" banner (shared/focus-hint.js, same mechanism ibmpc-at's
// own keyboard.spec.ts test covers) -- purely a web-UI convenience, no
// hardware equivalent, since a real Altair keyboard is just whatever
// terminal is wired to the serial port.

test.describe("focus hint", () => {
  test('"Click to focus" hint shows only while running and unfocused', async ({ page }) => {
    await boot(page);
    const hintVisible = () =>
      page.locator("#focusHint").evaluate((el) => el.classList.contains("visible"));
    // unlike ibmpc-at, this page's own boot() calls term.focus() right
    // away (see app.js), so the hint starts out hidden.
    expect(await hintVisible()).toBe(false);
    // clicking any other control blurs the terminal -- the hint appears
    await page.locator("#fullscreenBtn").focus();
    expect(await hintVisible()).toBe(true);
    await page.evaluate(() => (window as any).__test.term.focus());
    expect(await hintVisible()).toBe(false);
    // nothing to type into once powered off -- hidden regardless of focus
    await page.locator("#fullscreenBtn").focus();
    expect(await hintVisible()).toBe(true);
    await page.locator(".fp-power").click();
    expect(await hintVisible()).toBe(false);
  });
});
