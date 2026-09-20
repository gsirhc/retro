import { test, expect } from "./fixtures";
import { boot, waitForScreen, focusScreen, typeStr } from "./helpers";

// Fullscreen mode (expands #bezel -- CRT frame + vignette + power LED, not
// just the bare canvas -- see index.html's .bezel:fullscreen CSS comment),
// the corner Esc button, and the one-time hint dialog explaining why that
// button exists at all.
//
// The Esc button matters because browsers reserve the physical Esc key to
// exit fullscreen and never dispatch it to the page while doing so --
// confirmed live (not merely "also" exiting fullscreen: DOS gets nothing at
// all), so there is no way for page script to claim it back from the
// Fullscreen API. The button sends the real scancode directly instead,
// bypassing the native key event that problem lives in.
//
// FS_ESC_HINT_VERSION in app.js is "2" as of this writing; the tests below
// that plant a stored value hardcode that alongside a deliberately stale
// one, matching how a real visitor's browser would hold whatever version
// they last saw. Keep the "already seen" value here in sync if that
// constant changes again.

test.describe("fullscreen", () => {
  test("first-ever click shows the hint dialog and does not enter fullscreen yet", async ({ page }) => {
    await boot(page);
    await expect(page.locator("#fsEscHint")).toBeHidden();
    await page.locator("#fullscreenBtn").click();
    await expect(page.locator("#fsEscHint")).toBeVisible();
    expect(await page.evaluate(() => !!document.fullscreenElement)).toBe(false);
  });

  test("dismissing the hint enters fullscreen and reveals the Esc button", async ({ page }) => {
    await boot(page);
    await expect(page.locator("#escBtn")).toBeHidden();
    await page.locator("#fullscreenBtn").click();
    await page.locator("#fsEscHintOk").click();
    await expect.poll(() => page.evaluate(() => !!document.fullscreenElement)).toBe(true);
    await expect(page.locator("#fsEscHint")).toBeHidden();
    await expect(page.locator("#escBtn")).toBeVisible();
    await expect(page.locator("#fullscreenBtn")).toHaveAttribute("aria-label", "Exit fullscreen");
    // the stored flag now matches the current version, so it won't nag again
    expect(await page.evaluate(() => localStorage.getItem("retro8080.fsEscHintSeen"))).toBe("2");
  });

  test("hint does not reappear once the current version has already been seen", async ({ page }) => {
    await boot(page);
    await page.evaluate(() => localStorage.setItem("retro8080.fsEscHintSeen", "2"));
    await page.locator("#fullscreenBtn").click();
    await expect(page.locator("#fsEscHint")).toBeHidden();
    await expect.poll(() => page.evaluate(() => !!document.fullscreenElement)).toBe(true);
  });

  test("a stale stored version re-shows the hint -- the cache-bust", async ({ page }) => {
    await boot(page);
    await page.evaluate(() => localStorage.setItem("retro8080.fsEscHintSeen", "0"));
    await page.locator("#fullscreenBtn").click();
    await expect(page.locator("#fsEscHint")).toBeVisible();
    expect(await page.evaluate(() => !!document.fullscreenElement)).toBe(false);
  });

  test("exiting fullscreen hides the Esc button again and resets the toggle label", async ({ page }) => {
    await boot(page);
    await page.evaluate(() => localStorage.setItem("retro8080.fsEscHintSeen", "2"));
    await page.locator("#fullscreenBtn").click();
    await expect.poll(() => page.evaluate(() => !!document.fullscreenElement)).toBe(true);
    await page.locator("#fullscreenBtn").click();
    await expect.poll(() => page.evaluate(() => !!document.fullscreenElement)).toBe(false);
    await expect(page.locator("#escBtn")).toBeHidden();
    await expect(page.locator("#fullscreenBtn")).toHaveAttribute("aria-label", "Fullscreen");
  });

  test("the Esc button sends a real Escape to DOS -- clears a typed command line", async ({ page }) => {
    await boot(page);
    await page.evaluate(() => localStorage.setItem("retro8080.fsEscHintSeen", "2"));
    await page.locator("#fullscreenBtn").click();
    await expect.poll(() => page.evaluate(() => !!document.fullscreenElement)).toBe(true);
    await focusScreen(page);
    await typeStr(page, "ABC", { pressEnterAfter: false });
    await waitForScreen(page, /C:\\>abc/i);
    await page.locator("#escBtn").click();
    // COMMAND.COM's real Escape handling discards the pending line -- the
    // prompt is left bare again, not still holding "abc".
    await waitForScreen(page, /C:\\>\s*$/);
  });
});
