import { test, expect } from "./fixtures";
import { waitForScreen, focusScreen, typeStr } from "./helpers";

// Fullscreen expands #bezel (CRT frame, vignette, power LED), plus the corner Esc button and the
// one-time hint dialog. Browsers swallow the physical Esc key to exit fullscreen, so the button
// sends the scancode directly.
// FS_ESC_HINT_VERSION in app.js is "2"; tests planting a stored value hardcode it next to a stale
// one, so keep them in sync. Uses the shared livePage, which already clears retro8080.fsEscHintSeen.

test.describe("fullscreen", () => {
  test("first-ever click shows the hint dialog and does not enter fullscreen yet", async ({ livePage: page }) => {
    await expect(page.locator("#fsEscHint")).toBeHidden();
    await page.locator("#fullscreenBtn").click();
    await expect(page.locator("#fsEscHint")).toBeVisible();
    expect(await page.evaluate(() => !!document.fullscreenElement)).toBe(false);
  });

  test("dismissing the hint enters fullscreen and reveals the Esc button", async ({ livePage: page }) => {
    await expect(page.locator("#escBtn")).toBeHidden();
    await page.locator("#fullscreenBtn").click();
    await page.locator("#fsEscHintOk").click();
    await expect.poll(() => page.evaluate(() => !!document.fullscreenElement)).toBe(true);
    await expect(page.locator("#fsEscHint")).toBeHidden();
    await expect(page.locator("#escBtn")).toBeVisible();
    await expect(page.locator("#fullscreenBtn")).toHaveAttribute("aria-label", "Exit fullscreen");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.fsEscHintSeen"))).toBe("2");
  });

  test("hint does not reappear once the current version has already been seen", async ({ livePage: page }) => {
    await page.evaluate(() => localStorage.setItem("retro8080.fsEscHintSeen", "2"));
    await page.locator("#fullscreenBtn").click();
    await expect(page.locator("#fsEscHint")).toBeHidden();
    await expect.poll(() => page.evaluate(() => !!document.fullscreenElement)).toBe(true);
  });

  test("a stale stored version re-shows the hint -- the cache-bust", async ({ livePage: page }) => {
    await page.evaluate(() => localStorage.setItem("retro8080.fsEscHintSeen", "0"));
    await page.locator("#fullscreenBtn").click();
    await expect(page.locator("#fsEscHint")).toBeVisible();
    expect(await page.evaluate(() => !!document.fullscreenElement)).toBe(false);
  });

  test("exiting fullscreen hides the Esc button again and resets the toggle label", async ({ livePage: page }) => {
    await page.evaluate(() => localStorage.setItem("retro8080.fsEscHintSeen", "2"));
    await page.locator("#fullscreenBtn").click();
    await expect.poll(() => page.evaluate(() => !!document.fullscreenElement)).toBe(true);
    await page.locator("#fullscreenBtn").click();
    await expect.poll(() => page.evaluate(() => !!document.fullscreenElement)).toBe(false);
    await expect(page.locator("#escBtn")).toBeHidden();
    await expect(page.locator("#fullscreenBtn")).toHaveAttribute("aria-label", "Fullscreen");
  });

  test("the Esc button sends a real Escape to DOS -- clears a typed command line", async ({ promptPage: page }) => {
    await page.evaluate(() => localStorage.setItem("retro8080.fsEscHintSeen", "2"));
    await page.locator("#fullscreenBtn").click();
    await expect.poll(() => page.evaluate(() => !!document.fullscreenElement)).toBe(true);
    await focusScreen(page);
    await typeStr(page, "ABC", { pressEnterAfter: false });
    await waitForScreen(page, /C:\\>abc/i);
    await page.locator("#escBtn").click();
    // COMMAND.COM discards the pending line on Escape, leaving the prompt bare.
    await waitForScreen(page, /C:\\>\s*$/);
  });

  // Sound / mouse are bezel icons mirroring the checkboxes; KEYS mirrors Enable/Disable All.
  test("bezel sound/mouse/KEYS icons toggle sound, mouse, and all mappings", async ({ livePage: page }) => {
    await expect(page.locator("#speakerBtn")).toHaveAttribute("aria-pressed", "false");
    await expect(page.locator("#mouseCaptureBtn")).toHaveAttribute("aria-pressed", "false");
    await expect(page.locator("#keymapAllBtn")).toBeDisabled();

    await page.locator("#speakerBtn").click();
    await expect(page.locator("#speakerEnabled")).toBeChecked();
    await expect(page.locator("#speakerBtn")).toHaveAttribute("aria-pressed", "true");

    await page.locator("#mouseCaptureBtn").click();
    await expect(page.locator("#mouseCaptureEnabled")).toBeChecked();
    await expect(page.locator("#mouseCaptureBtn")).toHaveAttribute("aria-pressed", "true");

    await page.locator("#keymapPresetWasd").click();
    await expect(page.locator("#keymapAllBtn")).toBeEnabled();
    await expect(page.locator("#keymapAllBtn")).toHaveAttribute("aria-pressed", "true");
    await page.locator("#keymapAllBtn").click();
    await expect(page.locator("#keymapAllBtn")).toHaveAttribute("aria-pressed", "false");
    expect(await page.evaluate(() => (window as any).__test.mapKey("KeyW"))).toEqual(["KeyW"]);

    await page.locator("#speakerEnabled").uncheck();
    await expect(page.locator("#speakerBtn")).toHaveAttribute("aria-pressed", "false");
  });

  test("bezel toggles stay usable while fullscreen", async ({ livePage: page }) => {
    await page.locator("#keymapPresetWasd").click();
    await expect(page.locator("#keymapAllBtn")).toHaveAttribute("aria-pressed", "true");

    await page.evaluate(() => localStorage.setItem("retro8080.fsEscHintSeen", "2"));
    await page.locator("#fullscreenBtn").click();
    await expect.poll(() => page.evaluate(() => !!document.fullscreenElement)).toBe(true);

    await page.locator("#speakerBtn").click();
    await expect(page.locator("#speakerEnabled")).toBeChecked();
    await expect(page.locator("#speakerBtn")).toHaveAttribute("aria-pressed", "true");

    await page.locator("#keymapAllBtn").click();
    await expect(page.locator("#keymapAllBtn")).toHaveAttribute("aria-pressed", "false");
  });

  test("bezel button clicks in fullscreen return focus to the screen", async ({ livePage: page }) => {
    await page.locator("#keymapPresetWasd").click();
    await page.evaluate(() => localStorage.setItem("retro8080.fsEscHintSeen", "2"));
    await page.locator("#fullscreenBtn").click();
    await expect.poll(() => page.evaluate(() => !!document.fullscreenElement)).toBe(true);

    await page.locator("#screen").click();
    await expect(page.locator("#screen")).toBeFocused();

    await page.locator("#speakerBtn").click();
    await expect(page.locator("#screen")).toBeFocused();

    await page.locator("#keymapAllBtn").click();
    await expect(page.locator("#screen")).toBeFocused();

    await page.locator("#mouseCaptureBtn").click();
    await expect(page.locator("#screen")).toBeFocused();
  });

  test("the title-bar maximize button enters fullscreen like the bezel control", async ({ livePage: page }) => {
    await page.evaluate(() => localStorage.setItem("retro8080.fsEscHintSeen", "2"));
    await page.locator(".pb-max").click();
    await expect.poll(() => page.evaluate(() => !!document.fullscreenElement)).toBe(true);
    await expect(page.locator("#fullscreenBtn")).toHaveAttribute("aria-label", "Exit fullscreen");
  });

  test("the title-bar minimize button soft-collapses the page body", async ({ livePage: page }) => {
    await expect(page.locator(".inner")).toBeVisible();
    await page.locator(".pb-min").click();
    await expect(page.locator(".page")).toHaveClass(/is-minimized/);
    await expect(page.locator(".inner")).toBeHidden();
    await page.locator(".pb-min").click();
    await expect(page.locator(".inner")).toBeVisible();
  });

});
