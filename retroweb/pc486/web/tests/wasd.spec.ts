import { test, expect } from "./fixtures";
import { screenText } from "./helpers";

// Doom 1.2 and its contemporaries predate WASD -- they default to the arrow
// cluster. The mapping happens at the browser edge via the Key Mapper panel,
// so the guest receives genuine arrow-key scancodes. Off by default; the
// WASD → Arrows preset (and the bezel shortcut) turn it on.
test.describe("WASD to arrow keys", () => {
  test("preset persists across reload", async ({ page }) => {
    await page.goto("/?test=1&fast=1");
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 60_000 });
    await page.evaluate(() => (window as any).__test.clearKeymap());
    await expect(page.locator("#keymapAllBtn")).toBeDisabled();

    await page.locator("#keymapPresetWasd").click();
    await expect(page.locator("#keymapAllBtn")).toBeEnabled();
    await expect(page.locator("#keymapAllBtn")).toHaveAttribute("aria-pressed", "true");
    expect(await page.evaluate(() => (window as any).__test.isWasdPresetActive())).toBe(true);

    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 60_000 });
    await expect(page.locator("#keymapAllBtn")).toHaveAttribute("aria-pressed", "true");
    expect(await page.evaluate(() => (window as any).__test.mapKey("KeyW"))).toEqual(["ArrowUp"]);
    await page.evaluate(() => (window as any).__test.clearKeymap());
  });

  test("off: W types a literal w at the DOS prompt", async ({ promptPage: page }) => {
    await page.locator("#screen").click();
    await page.keyboard.press("w");
    await expect.poll(() => screenText(page), { timeout: 10_000 }).toMatch(/C:\\>w/);
  });

  test("on: W no longer types a w -- it becomes the up arrow", async ({ promptPage: page }) => {
    await page.locator("#screen").click();
    // FreeCom treats ArrowUp as command-history recall, not a no-op -- so
    // seed a distinctive entry, then map W→Up and expect that recall (not
    // the letter "w" typed onto a bare prompt).
    await page.keyboard.type("xyzzy");
    await page.keyboard.press("Enter");
    await expect.poll(() => screenText(page), { timeout: 10_000 }).toMatch(/xyzzy/i);
    await page.locator("#keymapPresetWasd").click();
    await page.locator("#screen").click();
    await page.keyboard.press("w");
    await expect
      .poll(async () => {
        const last = ((await screenText(page)).trimEnd().split(/\n/).pop() || "").trimEnd();
        return last;
      }, { timeout: 10_000 })
      .toMatch(/^C:\\>xyzzy$/i);
  });

  test("keys held across a toggle are released, not left stuck down", async ({ promptPage: page }) => {
    await page.locator("#screen").click();
    await page.keyboard.down("w");
    await page.locator("#keymapPresetWasd").click();
    await page.keyboard.up("w");
    // Whatever was held must have been broken; typing still works normally.
    await page.locator("#screen").click();
    await page.keyboard.press("x");
    await expect.poll(() => screenText(page), { timeout: 10_000 }).toMatch(/x/);
  });

  // A and D strafe rather than turn: Doom's strafe modifier is Alt
  // (key_strafe), so they send Alt with the arrow.
  test("A and D map to Alt+arrow so they strafe, W and S stay plain arrows", async ({
    promptPage: page,
  }) => {
    const map = async (code: string) =>
      page.evaluate((c) => (window as any).__test.mapKey(c), code);

    expect(await map("KeyA")).toEqual(["KeyA"]);   // off: untouched

    await page.locator("#keymapPresetWasd").click();
    expect(await map("KeyW")).toEqual(["ArrowUp"]);
    expect(await map("KeyS")).toEqual(["ArrowDown"]);
    expect(await map("KeyA")).toEqual(["AltLeft", "ArrowLeft"]);
    expect(await map("KeyD")).toEqual(["AltLeft", "ArrowRight"]);
  });

  test("on: A no longer types a literal a at the DOS prompt", async ({ promptPage: page }) => {
    await page.locator("#keymapPresetWasd").click();
    await page.locator("#screen").click();
    await page.keyboard.press("a");
    await page.waitForTimeout(500);
    expect((await screenText(page)).trimEnd().split(/\n/).pop() || "").not.toMatch(/a\s*$/i);
  });
});
