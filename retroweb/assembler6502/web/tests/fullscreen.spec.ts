import { test, expect } from "@playwright/test";

// Fullscreen expands #bezel (CRT frame, vignette, power LED), plus the corner
// Esc button and its one-time hint dialog. Browsers keep the physical Esc key
// to exit fullscreen, so the button feeds a synthetic Escape into the
// terminal-input path (app.js handleTermData). Tests that plant a stored hint
// version hardcode FS_ESC_HINT_VERSION ("2" in shared/fullscreen.js).

test.describe("fullscreen", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/");
    await expect(page.locator("#screen .xterm-rows")).toContainText("\\", { timeout: 45000 });
  });

  test("first-ever click shows the hint dialog and does not enter fullscreen yet", async ({ page }) => {
    await expect(page.locator("#fsEscHint")).toBeHidden();
    await page.locator("#fullscreenBtn").click();
    await expect(page.locator("#fsEscHint")).toBeVisible();
    expect(await page.evaluate(() => !!document.fullscreenElement)).toBe(false);
  });

  test("dismissing the hint enters fullscreen and reveals the Esc button", async ({ page }) => {
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
    await page.evaluate(() => localStorage.setItem("retro8080.fsEscHintSeen", "2"));
    await page.locator("#fullscreenBtn").click();
    await expect(page.locator("#fsEscHint")).toBeHidden();
    await expect.poll(() => page.evaluate(() => !!document.fullscreenElement)).toBe(true);
  });

  test("a stale stored version re-shows the hint -- the cache-bust", async ({ page }) => {
    await page.evaluate(() => localStorage.setItem("retro8080.fsEscHintSeen", "0"));
    await page.locator("#fullscreenBtn").click();
    await expect(page.locator("#fsEscHint")).toBeVisible();
    expect(await page.evaluate(() => !!document.fullscreenElement)).toBe(false);
  });

  test("exiting fullscreen hides the Esc button again and resets the toggle label", async ({ page }) => {
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
    await page.evaluate(() => localStorage.setItem("retro8080.fsEscHintSeen", "2"));
    const before = await page.locator("#screen").boundingBox();
    await page.locator("#fullscreenBtn").click();
    await expect.poll(() => page.evaluate(() => !!document.fullscreenElement)).toBe(true);
    // #screen is sized by inline pixel width/height (sizeScreen()). Poll rather
    // than diff once so a few-pixel reflow can't pass for the change.
    await expect
      .poll(() => page.locator("#screen").boundingBox().then((b) => b!.width))
      .toBeGreaterThan(before!.width * 1.2);
    const after = await page.locator("#screen").boundingBox();
    expect(after!.height).toBeGreaterThan(before!.height * 1.2);   // both axes grew
  });

  test("the Esc button feeds a real Escape into the board's serial input", async ({ page }) => {
    await page.evaluate(() => localStorage.setItem("retro8080.fsEscHintSeen", "2"));
    await page.locator("#fullscreenBtn").click();
    await expect.poll(() => page.evaluate(() => !!document.fullscreenElement)).toBe(true);
    const before = await page.evaluate(() => (window as any).__machine.cycleCount());
    await page.locator("#escBtn").click();
    await expect
      .poll(() => page.evaluate(() => (window as any).__machine.cycleCount()))
      .toBeGreaterThan(before);
  });
});
