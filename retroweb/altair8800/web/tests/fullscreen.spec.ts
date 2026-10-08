import { test, expect } from "./fixtures";
import { boot } from "./helpers";

// Fullscreen mode (expands #bezel), the corner Esc button, and its one-time hint dialog.
// Ported from ibmpc-at/web/tests/fullscreen.spec.ts. Browsers never dispatch a physical Esc
// while exiting fullscreen, so the button feeds a synthetic Escape into app.js handleTermData.
// Tests that plant a stored hint version hardcode FS_ESC_HINT_VERSION ("2") beside a stale one.

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
    // fullscreen grows the terminal's font and resizes #screen directly (no CSS transform).
    // Poll rather than diff once so an unrelated few-pixel reflow isn't mistaken for the change.
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
    // sendByte() feeds the 2SIO RX FIFO, same proof-of-delivery workflow.spec.ts uses
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.regs().cycles))
      .toBeGreaterThan(before);
  });

  test("the title-bar maximize button enters fullscreen like the bezel control", async ({ page }) => {
    await boot(page);
    await page.evaluate(() => localStorage.setItem("retro8080.fsEscHintSeen", "2"));
    await page.locator(".pb-max").click();
    await expect.poll(() => page.evaluate(() => !!document.fullscreenElement)).toBe(true);
    await expect(page.locator("#fullscreenBtn")).toHaveAttribute("aria-label", "Exit fullscreen");
  });

  test("the title-bar minimize button soft-collapses the page body", async ({ page }) => {
    await boot(page);
    await expect(page.locator(".inner")).toBeVisible();
    await page.locator(".pb-min").click();
    await expect(page.locator(".page")).toHaveClass(/is-minimized/);
    await expect(page.locator(".inner")).toBeHidden();
    await page.locator(".pb-min").click();
    await expect(page.locator(".inner")).toBeVisible();
  });

});
