import { test, expect } from "./fixtures";

// Key Mapper panel: custom rows, presets, enable/disable, delete, collapse,
// and persistence. Mapping itself is covered in wasd.spec.ts; this file is
// the panel chrome and Left Shift → Ctrl preset.
test.describe("Key Mapper panel", () => {
  test("Function keys and Key Mapper panels start expanded", async ({ livePage: page }) => {
    await expect(page.locator("#fkeysToggle")).toHaveAttribute("aria-expanded", "true");
    await expect(page.locator("#keymapToggle")).toHaveAttribute("aria-expanded", "true");
    await expect(page.locator("#fkeysCard")).not.toHaveClass(/collapsed/);
    await expect(page.locator("#keymapCard")).not.toHaveClass(/collapsed/);
    await expect(page.locator("#fkeysBody")).toBeVisible();
    await expect(page.locator("#keymapBody")).toBeVisible();
  });

  test("both panels collapse and expand from their headings", async ({ livePage: page }) => {
    await page.locator("#fkeysToggle").click();
    await expect(page.locator("#fkeysToggle")).toHaveAttribute("aria-expanded", "false");
    await expect(page.locator("#fkeysCard")).toHaveClass(/collapsed/);
    await expect(page.locator("#fkeysBody")).toBeHidden();

    await page.locator("#keymapToggle").click();
    await expect(page.locator("#keymapToggle")).toHaveAttribute("aria-expanded", "false");
    await expect(page.locator("#keymapCard")).toHaveClass(/collapsed/);
    await expect(page.locator("#keymapBody")).toBeHidden();

    await page.locator("#fkeysToggle").click();
    await expect(page.locator("#fkeysToggle")).toHaveAttribute("aria-expanded", "true");
    await expect(page.locator("#fkeysBody")).toBeVisible();

    await page.locator("#keymapToggle").click();
    await expect(page.locator("#keymapToggle")).toHaveAttribute("aria-expanded", "true");
    await expect(page.locator("#keymapBody")).toBeVisible();
  });

  test("panel collapse state persists across reload", async ({ page }) => {
    await page.goto("/?test=1&fast=1");
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 60_000 });
    await page.evaluate(() => localStorage.removeItem("retro8080.pc486.ui"));
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 60_000 });

    await page.locator("#fkeysToggle").click();
    await page.locator("#keymapToggle").click();
    await expect(page.locator("#fkeysToggle")).toHaveAttribute("aria-expanded", "false");
    await expect(page.locator("#keymapToggle")).toHaveAttribute("aria-expanded", "false");

    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 60_000 });
    await expect(page.locator("#fkeysToggle")).toHaveAttribute("aria-expanded", "false");
    await expect(page.locator("#keymapCard")).toHaveClass(/collapsed/);
    await expect(page.locator("#keymapBody")).toBeHidden();

    await page.evaluate(() => localStorage.removeItem("retro8080.pc486.ui"));
  });

  test("Left Shift → Ctrl preset maps ShiftLeft to ControlLeft", async ({ livePage: page }) => {
    await page.locator("#keymapPresetShiftCtrl").click();
    expect(await page.evaluate(() => (window as any).__test.mapKey("ShiftLeft"))).toEqual([
      "ControlLeft",
    ]);
    await expect(page.locator("#keymapTableBody tr")).toHaveCount(1);
    await expect(page.locator("#keymapTableBody tr").first()).toContainText("Left Shift");
    await expect(page.locator("#keymapTableBody tr").first()).toContainText("Left Ctrl");
  });

  test("WASD preset appears as a single table row", async ({ livePage: page }) => {
    await page.locator("#keymapPresetWasd").click();
    await expect(page.locator("#keymapTableBody tr")).toHaveCount(1);
    await expect(page.locator("#keymapTableBody tr").first()).toContainText("WASD");
    await expect(page.locator("#keymapTableBody tr").first()).toContainText("Arrows (A/D strafe)");
    expect(await page.evaluate(() => (window as any).__test.mapKey("KeyW"))).toEqual(["ArrowUp"]);
  });

  test("Disable All / Enable All toggles every mapping", async ({ livePage: page }) => {
    await page.locator("#keymapPresetWasd").click();
    await page.locator("#keymapPresetShiftCtrl").click();
    const toggle = page.locator("#keymapToggleAll");
    await expect(toggle).toHaveText("Disable All");
    await toggle.click();
    expect(await page.evaluate(() => (window as any).__test.mapKey("KeyW"))).toEqual(["KeyW"]);
    expect(await page.evaluate(() => (window as any).__test.mapKey("ShiftLeft"))).toEqual([
      "ShiftLeft",
    ]);
    await expect(toggle).toHaveText("Enable All");
    await toggle.click();
    expect(await page.evaluate(() => (window as any).__test.mapKey("KeyW"))).toEqual(["ArrowUp"]);
    expect(await page.evaluate(() => (window as any).__test.mapKey("ShiftLeft"))).toEqual([
      "ControlLeft",
    ]);
  });

  test("disable checkbox and delete remove the mapping effect", async ({ livePage: page }) => {
    await page.evaluate(() => {
      (window as any).__test.upsertMapping("KeyQ", ["KeyZ"], undefined);
    });
    expect(await page.evaluate(() => (window as any).__test.mapKey("KeyQ"))).toEqual(["KeyZ"]);

    const row = page.locator("#keymapTableBody tr").filter({ hasText: "Q" });
    await row.locator('input[type="checkbox"]').uncheck();
    expect(await page.evaluate(() => (window as any).__test.mapKey("KeyQ"))).toEqual(["KeyQ"]);

    await row.locator('input[type="checkbox"]').check();
    expect(await page.evaluate(() => (window as any).__test.mapKey("KeyQ"))).toEqual(["KeyZ"]);

    await row.getByRole("button", { name: "Delete" }).click();
    expect(await page.evaluate(() => (window as any).__test.mapKey("KeyQ"))).toEqual(["KeyQ"]);
    await expect(page.locator("#keymapTableBody tr.keymap-empty")).toHaveCount(1);
  });

  test("Add mapping captures From then To", async ({ livePage: page }) => {
    await page.locator("#keymapAddBtn").click();
    await expect(page.locator("#keymapCaptureStatus")).toContainText("physical key");
    await page.keyboard.press("m");
    await expect(page.locator("#keymapCaptureStatus")).toContainText("guest key");
    await page.keyboard.press("n");
    expect(await page.evaluate(() => (window as any).__test.mapKey("KeyM"))).toEqual(["KeyN"]);
    await expect(page.locator("#keymapAddBtn")).toHaveText("Add mapping");
    await expect(page.locator("#keymapCaptureStatus")).toHaveText("");
  });

  test("custom mapping persists across reload", async ({ page }) => {
    await page.goto("/?test=1&fast=1");
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 60_000 });
    await page.evaluate(() => {
      (window as any).__test.clearKeymap();
      (window as any).__test.upsertMapping("KeyH", ["KeyJ"], undefined);
    });
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 60_000 });
    expect(await page.evaluate(() => (window as any).__test.mapKey("KeyH"))).toEqual(["KeyJ"]);
    await page.evaluate(() => (window as any).__test.clearKeymap());
  });
});
