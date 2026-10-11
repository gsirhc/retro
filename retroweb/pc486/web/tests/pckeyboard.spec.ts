import { test, expect } from "./fixtures";
import { boardKey, focusScreen, openKeyboard, screenText, setPowerSwitch, waitForScreen } from "./helpers";

// The drawn IBM Enhanced Keyboard (shared/pc-keyboard.js): collapse state, clicks that reach DOS,
// latched modifiers, the lock lights, and map mode.
test.describe("drawn keyboard", () => {
  test("is hidden on a desktop until Keyboard is ticked, and remembers it", async ({ page }) => {
    await page.goto("/?test=1&fast=1");
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 60_000 });
    await page.evaluate(() => localStorage.removeItem("retro8080.pc486.keyboardOpen"));
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 60_000 });
    await expect(page.locator("#keyboardToggle")).not.toBeChecked();
    await expect(page.locator("#pcKeyboard")).toBeHidden();

    await page.locator("#keyboardToggle").check();
    await expect(page.locator("#pcKeyboard")).toBeVisible();
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 60_000 });
    await expect(page.locator("#keyboardToggle")).toBeChecked();
    await page.evaluate(() => localStorage.removeItem("retro8080.pc486.keyboardOpen"));
  });

  test("shows by default on a touch screen with no mouse", async ({ browser }) => {
    const ctx = await browser.newContext({ hasTouch: true, isMobile: true, viewport: { width: 820, height: 1180 } });
    const page = await ctx.newPage();
    await page.goto("/?test=1&fast=1");
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 60_000 });
    await expect(page.locator("#keyboardToggle")).toBeChecked();
    await ctx.close();
  });

  test("keys are inert while powered off", async ({ livePage: page }) => {
    await openKeyboard(page);
    await expect(boardKey(page, "F1")).toHaveAttribute("aria-disabled", "false");
    await setPowerSwitch(page, false);
    await expect(boardKey(page, "F1")).toHaveAttribute("aria-disabled", "true");
    await expect(page.locator("#pcKeyboard")).toHaveClass(/\boff\b/);
  });

  test("clicked keys type at the DOS prompt", async ({ promptPage: page }) => {
    await openKeyboard(page);
    for (const code of ["KeyV", "KeyE", "KeyR", "Enter"]) await boardKey(page, code).click();
    await waitForScreen(page, /FreeDOS/i);
  });

  test("a clicked key leaves focus on the screen", async ({ promptPage: page }) => {
    await openKeyboard(page);
    await focusScreen(page);
    await boardKey(page, "KeyQ").click();
    await expect(page.locator("#screen")).toBeFocused();
  });

  test("Shift latches for the next key, then lets go", async ({ promptPage: page }) => {
    await openKeyboard(page);
    await boardKey(page, "ShiftLeft").click();
    await expect(boardKey(page, "ShiftLeft")).toHaveClass(/latched/);
    await boardKey(page, "KeyQ").click();
    await expect(boardKey(page, "ShiftLeft")).not.toHaveClass(/latched/);
    await boardKey(page, "KeyQ").click();
    await expect.poll(() => screenText(page)).toMatch(/C:\\>Qq/);
  });

  test("clicking a latched modifier again releases it", async ({ livePage: page }) => {
    await openKeyboard(page);
    await boardKey(page, "ControlLeft").click();
    expect(await page.evaluate(() => (window as any).__test.keyboard.latched)).toEqual(["ControlLeft"]);
    await boardKey(page, "ControlLeft").click();
    expect(await page.evaluate(() => (window as any).__test.keyboard.latched)).toEqual([]);
    expect(await page.evaluate(() => (window as any).__test.heldKeysSize)).toBe(0);
  });

  test("Caps Lock lights its lamp once the BIOS sets the LEDs", async ({ promptPage: page }) => {
    await openKeyboard(page);
    const lamp = page.locator('#pcKeyboard .kbd-led[data-led="2"]');
    await expect(lamp).not.toHaveClass(/\bon\b/);
    await boardKey(page, "CapsLock").click();
    await expect(lamp).toHaveClass(/\bon\b/);
    await boardKey(page, "CapsLock").click();
    await expect(lamp).not.toHaveClass(/\bon\b/);
  });

  test("a real key press shows on the drawn key", async ({ livePage: page }) => {
    await openKeyboard(page);
    await focusScreen(page);
    await page.keyboard.down("KeyG");
    await expect(boardKey(page, "KeyG")).toHaveClass(/\bdown\b/);
    await page.keyboard.up("KeyG");
    await expect(boardKey(page, "KeyG")).not.toHaveClass(/\bdown\b/);
  });

  test("F-keys and the editing keys don't desync the keyboard", async ({ promptPage: page }) => {
    await openKeyboard(page);
    for (const code of ["F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
      "Insert", "Delete", "Home", "End", "PageUp", "PageDown", "PrintScreen", "ScrollLock", "Pause", "NumLock"]) {
      await boardKey(page, code).click();
    }
    await boardKey(page, "Escape").click();
    for (const code of ["KeyV", "KeyE", "KeyR", "Enter"]) await boardKey(page, code).click();
    await waitForScreen(page, /FreeDOS/i);
  });

  test("holding a clicked key repeats on the keyboard's own clock", async ({ promptPage: page }) => {
    await openKeyboard(page);
    const box = (await boardKey(page, "KeyX").boundingBox())!;
    await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
    await page.mouse.down();
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.machine.keyboardRepeating()))
      .toBe(true);
    await expect.poll(async () => (/C:\\>(x+)/.exec(await screenText(page)) || ["", ""])[1].length)
      .toBeGreaterThan(3);
    await page.mouse.up();
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.machine.keyboardRepeating()))
      .toBe(false);
  });

  test("losing focus lets go of a latched modifier", async ({ livePage: page }) => {
    await openKeyboard(page);
    await boardKey(page, "ControlLeft").click();
    await boardKey(page, "AltLeft").click();
    expect(await page.evaluate(() => (window as any).__test.heldKeysSize)).toBe(2);
    await page.evaluate(() => window.dispatchEvent(new Event("blur")));
    expect(await page.evaluate(() => (window as any).__test.keyboard.latched)).toEqual([]);
    expect(await page.evaluate(() => (window as any).__test.heldKeysSize)).toBe(0);
    await expect(boardKey(page, "ControlLeft")).not.toHaveClass(/latched/);
  });

  test("the lock lights go dark at power-off", async ({ promptPage: page }) => {
    await openKeyboard(page);
    const lamp = page.locator('#pcKeyboard .kbd-led[data-led="2"]');
    await boardKey(page, "CapsLock").click();
    await expect(lamp).toHaveClass(/\bon\b/);
    await setPowerSwitch(page, false);
    await expect(lamp).not.toHaveClass(/\bon\b/);
  });
});

test.describe("map mode", () => {
  const mapKey = (page: any, code: string) => page.evaluate((c: string) => (window as any).__test.mapKey(c), code);

  test("a real key, then a clicked target", async ({ livePage: page }) => {
    await openKeyboard(page);
    await page.locator("#keymapAddBtn").click();
    await expect(page.locator("#keymapCaptureStatus")).toContainText("Press the key");
    await page.keyboard.press("m");
    await expect(boardKey(page, "KeyM")).toHaveClass(/map-from/);
    await boardKey(page, "KeyN").click();
    expect(await mapKey(page, "KeyM")).toEqual(["KeyN"]);
    await expect(boardKey(page, "KeyM").locator(".kbd-map-txt")).toHaveText("\u2192N");
    await expect(page.locator("#keymapAddBtn")).toHaveText("Map a key");
  });

  test("a clicked source and a latched chord", async ({ livePage: page }) => {
    await openKeyboard(page);
    await page.locator("#keymapAddBtn").click();
    await boardKey(page, "KeyQ").click();
    await boardKey(page, "AltLeft").click();
    await expect(page.locator("#keymapAddBtn")).toHaveText("Done");
    await boardKey(page, "ArrowLeft").click();
    expect(await mapKey(page, "KeyQ")).toEqual(["AltLeft", "ArrowLeft"]);
  });

  test("a modifier on its own, finished with Done", async ({ livePage: page }) => {
    await openKeyboard(page);
    await page.locator("#keymapAddBtn").click();
    await boardKey(page, "CapsLock").click();
    await boardKey(page, "ControlLeft").click();
    await page.locator("#keymapAddBtn").click();
    expect(await mapKey(page, "CapsLock")).toEqual(["ControlLeft"]);
  });

  test("clicking a mapped key twice clears it", async ({ livePage: page }) => {
    await openKeyboard(page);
    await page.evaluate(() => (window as any).__test.upsertMapping("KeyH", ["KeyJ"], undefined));
    await expect(boardKey(page, "KeyH")).toHaveClass(/\bmapped\b/);
    await page.locator("#keymapAddBtn").click();
    await boardKey(page, "KeyH").click();
    await expect(page.locator("#keymapCaptureStatus")).toContainText("Click it again");
    await boardKey(page, "KeyH").click();
    expect(await mapKey(page, "KeyH")).toEqual(["KeyH"]);
    await expect(boardKey(page, "KeyH")).not.toHaveClass(/\bmapped\b/);
  });

  test("Esc and Cancel leave map mode without a mapping", async ({ livePage: page }) => {
    await openKeyboard(page);
    await page.locator("#keymapAddBtn").click();
    await page.keyboard.press("Escape");
    await expect(page.locator("#keymapAddBtn")).toHaveAttribute("aria-pressed", "false");
    await page.locator("#keymapAddBtn").click();
    await boardKey(page, "KeyZ").click();
    await page.locator("#keymapAddBtn").click();
    await expect(page.locator("#keymapAddBtn")).toHaveText("Map a key");
    expect(await page.evaluate(() => (window as any).__test.keymapRows)).toEqual([]);
  });

  test("clicked keys follow mappings too", async ({ promptPage: page }) => {
    await openKeyboard(page);
    await page.evaluate(() => (window as any).__test.upsertMapping("KeyQ", ["KeyZ"], undefined));
    await boardKey(page, "KeyQ").click();
    await expect.poll(() => screenText(page)).toMatch(/C:\\>z/);
  });

  test("a clicked key mapped to a modifier latches that modifier", async ({ livePage: page }) => {
    await openKeyboard(page);
    await page.locator("#keymapPresetShiftCtrl").click();
    await boardKey(page, "ShiftLeft").click();
    await expect(boardKey(page, "ShiftLeft")).toHaveClass(/latched/);
    await expect(boardKey(page, "ControlLeft")).toHaveClass(/\bdown\b/);
    await boardKey(page, "ShiftLeft").click();
    await expect(boardKey(page, "ControlLeft")).not.toHaveClass(/\bdown\b/);
    expect(await page.evaluate(() => (window as any).__test.heldKeysSize)).toBe(0);
  });

  test("a real key, then a real combination as the target", async ({ livePage: page }) => {
    await openKeyboard(page);
    await page.locator("#keymapAddBtn").click();
    await page.keyboard.press("q");
    await page.keyboard.down("Alt");
    await page.keyboard.press("ArrowLeft");
    await page.keyboard.up("Alt");
    expect(await mapKey(page, "KeyQ")).toEqual(["AltLeft", "ArrowLeft"]);
    await expect(page.locator("#keymapAddBtn")).toHaveText("Map a key");
  });

  test("a real modifier on its own as the target lands on its release", async ({ livePage: page }) => {
    await openKeyboard(page);
    await page.locator("#keymapAddBtn").click();
    await page.keyboard.press("CapsLock");
    await page.keyboard.down("ShiftLeft");
    await expect(page.locator("#keymapAddBtn")).toHaveAttribute("aria-pressed", "true");
    await page.keyboard.up("ShiftLeft");
    expect(await mapKey(page, "CapsLock")).toEqual(["ShiftLeft"]);
  });

  test("map mode works with the machine off", async ({ livePage: page }) => {
    await setPowerSwitch(page, false);
    await openKeyboard(page);
    await page.locator("#keymapAddBtn").click();
    await boardKey(page, "KeyQ").click();
    await boardKey(page, "KeyZ").click();
    expect(await mapKey(page, "KeyQ")).toEqual(["KeyZ"]);
  });

  test("a mapping's tag switches it off and on, its × deletes it", async ({ livePage: page }) => {
    await openKeyboard(page);
    const mapKey = (c: string) => page.evaluate((k) => (window as any).__test.mapKey(k), c);
    await page.evaluate(() => (window as any).__test.upsertMapping("KeyN", ["KeyL"], undefined));
    const tag = boardKey(page, "KeyN").locator(".kbd-map-txt");
    await tag.click();
    expect(await mapKey("KeyN")).toEqual(["KeyN"]);
    await expect(boardKey(page, "KeyN")).toHaveClass(/mapped-off/);
    await expect(tag).toHaveAttribute("aria-pressed", "false");
    expect(await page.evaluate(() => (window as any).__test.heldKeysSize)).toBe(0);
    await tag.click();
    expect(await mapKey("KeyN")).toEqual(["KeyL"]);
    await expect(boardKey(page, "KeyN")).not.toHaveClass(/mapped-off/);
    await boardKey(page, "KeyN").locator(".kbd-map-del").click();
    expect(await mapKey("KeyN")).toEqual(["KeyN"]);
    await expect(boardKey(page, "KeyN")).not.toHaveClass(/\bmapped\b/);
    expect(await page.evaluate(() => (window as any).__test.keymapRows)).toEqual([]);
  });

  test("a switched-off mapping stays off across reload", async ({ page }) => {
    await page.goto("/?test=1&fast=1");
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 60_000 });
    await page.evaluate(() => localStorage.removeItem("retro8080.pc486.keymap"));
    await page.evaluate(() => (window as any).__test.upsertMapping("KeyN", ["KeyL"], undefined));
    await openKeyboard(page);
    await boardKey(page, "KeyN").locator(".kbd-map-txt").click();
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 60_000 });
    expect(await page.evaluate(() => (window as any).__test.mapKey("KeyN"))).toEqual(["KeyN"]);
    await openKeyboard(page);
    await expect(boardKey(page, "KeyN")).toHaveClass(/mapped-off/);
    await page.evaluate(() => (window as any).__test.clearKeymap());
  });

  test("Clear All removes every mapping", async ({ livePage: page }) => {
    await openKeyboard(page);
    await page.locator("#keymapPresetWasd").click();
    await expect(boardKey(page, "KeyW").locator(".kbd-map-txt")).toHaveText("\u2192\u2191");
    await page.locator("#keymapClearBtn").click();
    expect(await mapKey(page, "KeyW")).toEqual(["KeyW"]);
    await expect(page.locator("#keymapClearBtn")).toBeDisabled();
  });
});
