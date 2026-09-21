import { test, expect } from "@playwright/test";

// "Click to focus" banner (shared/focus-hint.js) -- a pure web-UI
// convenience, no arcade-cabinet equivalent. Unlike the xterm-based
// machines, this page's keyboard listeners are attached to `window` (see
// app.js's applyKeys), so gameplay input works regardless of DOM focus --
// the hint is purely a visual nicety, and app.js never calls screen.focus()
// itself, so the hint starts out visible on a fresh load.

test.describe("focus hint", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/");
    await expect(page.locator("#screen")).toBeVisible();
  });

  const hintVisible = (page: import("@playwright/test").Page) =>
    page.locator("#focusHint").evaluate((el) => el.classList.contains("visible"));

  test('shows on load (canvas starts unfocused), hides once clicked', async ({ page }) => {
    expect(await hintVisible(page)).toBe(true);
    await page.locator("#screen").click();
    expect(await hintVisible(page)).toBe(false);
  });

  test("reappears once focus moves to another control", async ({ page }) => {
    await page.locator("#screen").click();
    expect(await hintVisible(page)).toBe(false);
    await page.locator("#fullscreenBtn").focus();
    expect(await hintVisible(page)).toBe(true);
  });
});
