import { test, expect } from "./fixtures";
import { screenText } from "./helpers";

// Doom 1.2 and its contemporaries predate WASD -- they default to the arrow
// cluster. The mapping happens at the browser edge, so the guest receives
// genuine arrow-key scancodes; it is off by default, like every other
// convenience control on this page.
test.describe("WASD to arrow keys", () => {
  test("is off by default and never restored from a saved preference", async ({ page }) => {
    await page.goto("/?test=1");
    const box = page.locator("#wasdArrows");
    await expect(box).not.toBeChecked();
    await box.check();
    await page.reload();
    await expect(page.locator("#wasdArrows")).not.toBeChecked();
  });

  test("off: W types a literal w at the DOS prompt", async ({ promptPage: page }) => {
    await page.locator("#screen").click();
    await page.keyboard.press("w");
    await expect.poll(() => screenText(page), { timeout: 10_000 }).toMatch(/C:\\>w/);
  });

  test("on: W no longer types a w -- it becomes the up arrow", async ({ promptPage: page }) => {
    await page.locator("#wasdArrows").check();
    await page.locator("#screen").click();
    await page.keyboard.press("w");
    await page.keyboard.press("a");
    await page.keyboard.press("s");
    await page.keyboard.press("d");
    // None of the four may reach the guest as a character. Arrow keys at a
    // bare prompt do not echo, so the command line must stay empty.
    await page.waitForTimeout(500);
    expect(await screenText(page)).not.toMatch(/C:\\>[wasd]/);
  });

  test("keys held across a toggle are released, not left stuck down", async ({ promptPage: page }) => {
    await page.locator("#screen").click();
    await page.keyboard.down("w");            // tracked as ArrowUp once mapped
    await page.locator("#wasdArrows").check();
    await page.keyboard.up("w");
    // Whatever was held must have been broken; typing still works normally.
    await page.locator("#screen").click();
    await page.keyboard.press("x");
    await expect.poll(() => screenText(page), { timeout: 10_000 }).toMatch(/x/);
  });
});
