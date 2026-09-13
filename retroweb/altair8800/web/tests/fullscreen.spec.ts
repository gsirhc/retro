import { test, expect } from "./fixtures";
import { boot } from "./helpers";

// Fullscreen mode (expands #bezel -- CRT frame + vignette + power LED, not
// just the bare terminal -- see index.html's .bezel:fullscreen CSS comment),
// the corner Esc button, and the one-time hint dialog explaining why that
// button exists at all. Ported from ibmpc-at/web/tests/fullscreen.spec.ts
// (shared/fullscreen.js is the same mechanism on both pages).
//
// The Esc button matters because browsers reserve the physical Esc key to
// exit fullscreen and never dispatch it to the page while doing so, so
// there is no way for page script to claim it back from the Fullscreen
// API. The button feeds a synthetic Escape straight into the machine's own
// terminal-input path instead (see app.js's handleTermData), bypassing the
// native key event that problem lives in.
//
// FS_ESC_HINT_VERSION in shared/fullscreen.js is "2" as of this writing;
// the tests below that plant a stored value hardcode that alongside a
// deliberately stale one, matching how a real visitor's browser would hold
// whatever version they last saw.

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

  test("entering fullscreen scales the terminal up to fill the screen, keeping its aspect ratio", async ({ page }) => {
    await page.setViewportSize({ width: 1600, height: 900 });
    await boot(page);
    await page.evaluate(() => localStorage.setItem("retro8080.fsEscHintSeen", "2"));
    const before = await page.locator("#screen").boundingBox();
    await page.locator("#fullscreenBtn").click();
    await expect.poll(() => page.evaluate(() => !!document.fullscreenElement)).toBe(true);
    // #screen is a <div> sized by inline pixel width/height (see
    // sizeScreen()) -- entering fullscreen grows the terminal's own font
    // size and resizes #screen to fill the bezel directly, not a CSS
    // transform (a different machine's mechanism). Poll rather than diff a
    // single before/after pair so an unrelated few-pixel reflow (e.g. the
    // page's own scrollbar disappearing once fullscreen hides the
    // document) can't be mistaken for the real change.
    await expect
      .poll(() => page.locator("#screen").boundingBox().then((b) => b!.width))
      .toBeGreaterThan(before!.width * 1.2);
    const after = await page.locator("#screen").boundingBox();
    expect(after!.height).toBeGreaterThan(before!.height * 1.2);   // both axes grew
  });

  test("the Esc button feeds a real Escape into the machine's serial input", async ({ page }) => {
    await boot(page);
    await page.evaluate(() => localStorage.setItem("retro8080.fsEscHintSeen", "2"));
    await page.locator("#fullscreenBtn").click();
    await expect.poll(() => page.evaluate(() => !!document.fullscreenElement)).toBe(true);
    const before = await page.evaluate(() => (window as any).__test.regs().cycles);
    await page.locator("#escBtn").click();
    // sendByte() feeds the 2SIO RX FIFO the CPU polls -- some cycles run to
    // service it, same observable proof-of-delivery workflow.spec.ts's own
    // preset tests use rather than asserting on a specific guest reaction.
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.regs().cycles))
      .toBeGreaterThan(before);
  });
});
