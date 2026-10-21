import { test, expect } from "./fixtures";
import { boardKey, openKeyboard, withFull } from "./helpers";

// Key Mapper presets, bulk toggles and persistence. Map mode itself is in pckeyboard.spec.ts.
test.describe("Key Mapper", () => {
  const mapKey = (page: any, code: string) => page.evaluate((c: string) => (window as any).__test.mapKey(c), code);

  test("Left Shift → Ctrl preset maps ShiftLeft to ControlLeft", async ({ livePage: page }) => {
    await openKeyboard(page);
    await page.locator("#keymapPresetShiftCtrl").click();
    expect(await mapKey(page, "ShiftLeft")).toEqual(["ControlLeft"]);
    await expect(boardKey(page, "ShiftLeft")).toHaveAttribute("title", /Left Shift key sends Left Ctrl/);
  });

  test("WASD preset badges all four keys", async ({ livePage: page }) => {
    await openKeyboard(page);
    await page.locator("#keymapPresetWasd").click();
    for (const code of ["KeyW", "KeyA", "KeyS", "KeyD"]) {
      await expect(boardKey(page, code)).toHaveClass(/\bmapped\b/);
    }
    expect(await mapKey(page, "KeyW")).toEqual(["ArrowUp"]);
  });

  test("Disable All / Enable All toggles every mapping", async ({ livePage: page }) => {
    await openKeyboard(page);
    await page.locator("#keymapPresetWasd").click();
    await page.locator("#keymapPresetShiftCtrl").click();
    const toggle = page.locator("#keymapToggleAll");
    await expect(toggle).toHaveText("Disable All");
    await toggle.click();
    expect(await mapKey(page, "KeyW")).toEqual(["KeyW"]);
    expect(await mapKey(page, "ShiftLeft")).toEqual(["ShiftLeft"]);
    await expect(boardKey(page, "KeyW")).toHaveClass(/mapped-off/);
    await expect(toggle).toHaveText("Enable All");
    await toggle.click();
    expect(await mapKey(page, "KeyW")).toEqual(["ArrowUp"]);
    expect(await mapKey(page, "ShiftLeft")).toEqual(["ControlLeft"]);
  });

  test("the KEYS bezel button mirrors Disable All", async ({ livePage: page }) => {
    const keys = page.locator("#keymapAllBtn");
    await expect(keys).toBeDisabled();
    await page.evaluate(() => (window as any).__test.applyWasdPreset());
    await expect(keys).toHaveAttribute("aria-pressed", "true");
    await keys.click();
    await expect(keys).toHaveAttribute("aria-pressed", "false");
    expect(await mapKey(page, "KeyW")).toEqual(["KeyW"]);
  });

  test("custom mapping persists across reload", async ({ page }) => {
    await page.goto(withFull("/?test=1&fast=1"));
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 60_000 });
    await page.evaluate(() => {
      (window as any).__test.clearKeymap();
      (window as any).__test.upsertMapping("KeyH", ["KeyJ"], undefined);
    });
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 60_000 });
    expect(await mapKey(page, "KeyH")).toEqual(["KeyJ"]);
    await page.evaluate(() => (window as any).__test.clearKeymap());
  });
});
