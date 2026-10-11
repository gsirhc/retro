import { test, expect } from "./fixtures";
import { boot, boardKey, focusScreen, openKeyboard, screenText, setPowerSwitch, waitForScreen } from "./helpers";

// The drawn IBM Enhanced Keyboard (shared/pc-keyboard.js) on the AT: collapse state, latched
// modifiers, the lock lights, and map mode.
test.describe("drawn keyboard", () => {
  test("is hidden on a desktop until Keyboard is ticked, and remembers it", async ({ page }) => {
    await boot(page, { expectScreen: null });
    await expect(page.locator("#keyboardToggle")).not.toBeChecked();
    await expect(page.locator("#pcKeyboard")).toBeHidden();
    await page.locator("#keyboardToggle").check();
    await expect(page.locator("#pcKeyboard")).toBeVisible();
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 60_000 });
    await expect(page.locator("#keyboardToggle")).toBeChecked();
  });

  test("shows by default on a touch screen with no mouse", async ({ browser }) => {
    const ctx = await browser.newContext({ hasTouch: true, isMobile: true, viewport: { width: 820, height: 1180 } });
    const page = await ctx.newPage();
    await boot(page, { expectScreen: null });
    await expect(page.locator("#keyboardToggle")).toBeChecked();
    await ctx.close();
  });

  test("clicked keys type, and Shift latches for one key", async ({ page }) => {
    await boot(page);
    await openKeyboard(page);
    await focusScreen(page);
    await boardKey(page, "ShiftLeft").click();
    await expect(boardKey(page, "ShiftLeft")).toHaveClass(/latched/);
    await boardKey(page, "KeyQ").click();
    await expect(boardKey(page, "ShiftLeft")).not.toHaveClass(/latched/);
    await boardKey(page, "KeyQ").click();
    await expect.poll(() => screenText(page)).toMatch(/C:\\>Qq/);
    await expect(page.locator("#screen")).toBeFocused();
  });

  test("Num Lock lights its lamp once the BIOS sets the LEDs", async ({ page }) => {
    await boot(page);
    await openKeyboard(page);
    const lamp = page.locator('#pcKeyboard .kbd-led[data-led="1"]');
    const before = await lamp.evaluate((el) => el.classList.contains("on"));
    await boardKey(page, "NumLock").click();
    await expect.poll(() => lamp.evaluate((el) => el.classList.contains("on"))).toBe(!before);
  });

  test("a real key press shows on the drawn key", async ({ page }) => {
    await boot(page, { expectScreen: null });
    await openKeyboard(page);
    await focusScreen(page);
    await page.keyboard.down("KeyG");
    await expect(boardKey(page, "KeyG")).toHaveClass(/\bdown\b/);
    await page.keyboard.up("KeyG");
    await expect(boardKey(page, "KeyG")).not.toHaveClass(/\bdown\b/);
  });

  test("holding a clicked key repeats on the keyboard's own clock", async ({ page }) => {
    await boot(page);
    await openKeyboard(page);
    const box = (await boardKey(page, "KeyX").boundingBox())!;
    await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
    await page.mouse.down();
    await waitForScreen(page, /C:\\>x{4,}/i, 20_000);
    await page.mouse.up();
  });

  test("losing focus lets go of a latched modifier", async ({ page }) => {
    await boot(page, { expectScreen: null });
    await openKeyboard(page);
    await focusScreen(page);
    await boardKey(page, "ControlLeft").click();
    expect(await page.evaluate(() => (window as any).__test.keyboard.heldKeysSize)).toBe(1);
    await page.evaluate(() => (window as any).__test.screenEl.blur());
    expect(await page.evaluate(() => (window as any).__test.keyboard.latched)).toEqual([]);
    expect(await page.evaluate(() => (window as any).__test.keyboard.heldKeysSize)).toBe(0);
  });

  test("the lock lights go dark at power-off", async ({ page }) => {
    await boot(page);
    await openKeyboard(page);
    const lamp = page.locator('#pcKeyboard .kbd-led[data-led="2"]');
    await boardKey(page, "CapsLock").click();
    await expect(lamp).toHaveClass(/\bon\b/);
    await setPowerSwitch(page, false);
    await expect(lamp).not.toHaveClass(/\bon\b/);
  });
});

test.describe("map mode", () => {
  test("a real key mapped to a clicked key types the new key", async ({ page }) => {
    await boot(page);
    await openKeyboard(page);
    await page.locator("#keymapAddBtn").click();
    await page.keyboard.press("m");
    await boardKey(page, "KeyN").click();
    await expect(boardKey(page, "KeyM").locator(".kbd-map-txt")).toHaveText("\u2192N");
    await focusScreen(page);
    await page.keyboard.down("KeyM");
    await page.waitForTimeout(60);
    await page.keyboard.up("KeyM");
    // fast=1 makes the 60 ms hold long enough for typematic, so n may repeat.
    await waitForScreen(page, /C:\\>n+\s*$/i);
  });

  test("a real combination, or a real modifier alone, as the target", async ({ page }) => {
    await boot(page, { expectScreen: null });
    await openKeyboard(page);
    const mapKey = (c: string) => page.evaluate((k) => (window as any).__test.keyboard.mapKey(k), c);
    await page.locator("#keymapAddBtn").click();
    await page.keyboard.press("q");
    await page.keyboard.down("Alt");
    await page.keyboard.press("ArrowLeft");
    await page.keyboard.up("Alt");
    expect(await mapKey("KeyQ")).toEqual(["AltLeft", "ArrowLeft"]);
    await page.locator("#keymapAddBtn").click();
    await page.keyboard.press("CapsLock");
    await page.keyboard.down("ShiftLeft");
    await page.keyboard.up("ShiftLeft");
    expect(await mapKey("CapsLock")).toEqual(["ShiftLeft"]);
  });

  test("map mode works with the machine off", async ({ page }) => {
    await boot(page, { expectScreen: null });
    await setPowerSwitch(page, false);
    await openKeyboard(page);
    await expect(boardKey(page, "KeyQ")).toHaveAttribute("aria-disabled", "true");
    await page.locator("#keymapAddBtn").click();
    await expect(boardKey(page, "KeyQ")).toHaveAttribute("aria-disabled", "false");
    await boardKey(page, "KeyQ").click();
    await boardKey(page, "KeyZ").click();
    expect(await page.evaluate(() => (window as any).__test.keyboard.mapKey("KeyQ"))).toEqual(["KeyZ"]);
    await expect(boardKey(page, "KeyQ")).toHaveAttribute("aria-disabled", "true");
  });

  test("map-mode details: Esc, unlatching a modifier, clearing by clicking twice", async ({ page }) => {
    await boot(page, { expectScreen: null });
    await openKeyboard(page);
    const map = page.locator("#keymapAddBtn");
    const mapKey = (c: string) => page.evaluate((k) => (window as any).__test.keyboard.mapKey(k), c);
    await map.click();
    await page.keyboard.press("Escape");
    await expect(map).toHaveAttribute("aria-pressed", "false");

    await map.click();
    await boardKey(page, "KeyQ").click();
    await boardKey(page, "ShiftLeft").click();
    await expect(map).toHaveText("Done");
    await boardKey(page, "ShiftLeft").click();
    await expect(map).toHaveText("Cancel");
    await boardKey(page, "KeyZ").click();
    expect(await mapKey("KeyQ")).toEqual(["KeyZ"]);

    await map.click();
    await boardKey(page, "KeyQ").click();
    await boardKey(page, "KeyQ").click();
    expect(await mapKey("KeyQ")).toEqual(["KeyQ"]);
  });

  test("presets and Disable All from the bar", async ({ page }) => {
    await boot(page, { expectScreen: null });
    await openKeyboard(page);
    const mapKey = (c: string) => page.evaluate((k) => (window as any).__test.keyboard.mapKey(k), c);
    await page.locator("#keymapPresetShiftCtrl").click();
    expect(await mapKey("ShiftLeft")).toEqual(["ControlLeft"]);
    await page.locator("#keymapToggleAll").click();
    await expect(page.locator("#keymapToggleAll")).toHaveText("Enable All");
    expect(await mapKey("ShiftLeft")).toEqual(["ShiftLeft"]);
    await page.locator("#keymapToggleAll").click();
    expect(await mapKey("ShiftLeft")).toEqual(["ControlLeft"]);
  });

  test("A and D held together keep the shared Alt down until both let go", async ({ page }) => {
    await boot(page, { expectScreen: null });
    await openKeyboard(page);
    await page.locator("#keymapPresetWasd").click();
    await focusScreen(page);
    await page.keyboard.down("KeyA");
    await page.keyboard.down("KeyD");
    await page.keyboard.up("KeyA");
    await expect(boardKey(page, "AltLeft")).toHaveClass(/\bdown\b/);
    await expect(boardKey(page, "ArrowLeft")).not.toHaveClass(/\bdown\b/);
    await page.keyboard.up("KeyD");
    await expect(boardKey(page, "AltLeft")).not.toHaveClass(/\bdown\b/);
    expect(await page.evaluate(() => (window as any).__test.keyboard.heldKeysSize)).toBe(0);
  });

  test("saved mappings that don't parse are dropped on load", async ({ page }) => {
    await boot(page, { expectScreen: null });
    await page.evaluate(() => localStorage.setItem("retro8080.ibmpcat.keymap", JSON.stringify([
      { id: 4, from: "KeyH", to: ["KeyJ"], enabled: false, preset: "bogus" },
      { from: "KeyK", to: ["KeyL"] },
      null, { from: 7, to: ["KeyA"] }, { from: "KeyB", to: [] }, { from: "KeyC", to: [3] },
    ])));
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine);
    const rows = await page.evaluate(() => (window as any).__test.keyboard.keymapRows);
    expect(rows).toEqual([
      { id: 4, from: "KeyH", to: ["KeyJ"], enabled: false, preset: undefined },
      { id: 5, from: "KeyK", to: ["KeyL"], enabled: true, preset: undefined },
    ]);
    await page.evaluate(() => localStorage.setItem("retro8080.ibmpcat.keymap", "not json"));
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine);
    expect(await page.evaluate(() => (window as any).__test.keyboard.keymapRows)).toEqual([]);
  });

  test("clicking a mapped key sends its whole mapping, in order", async ({ page }) => {
    await boot(page, { expectScreen: null });
    await openKeyboard(page);
    await page.locator("#keymapPresetWasd").click();
    await page.evaluate(() => {
      const m = (window as any).__test.machine;
      const inject = m.injectScancode.bind(m);
      (window as any).__sent = [];
      m.injectScancode = (code: number) => { (window as any).__sent.push(code); inject(code); };
    });
    await boardKey(page, "KeyA").click();
    // Alt make, Left make, Left break, Alt break.
    await expect.poll(() => page.evaluate(() => (window as any).__sent))
      .toEqual([0x38, 0xe0, 0x4b, 0xe0, 0xcb, 0xb8]);
    expect(await page.evaluate(() => (window as any).__test.keyboard.heldKeysSize)).toBe(0);
  });

  test("a clicked N mapped to L types l", async ({ page }) => {
    await boot(page);
    await openKeyboard(page);
    await page.evaluate(() => (window as any).__test.keyboard.upsertMapping("KeyN", ["KeyL"], undefined));
    await boardKey(page, "KeyN").click();
    await waitForScreen(page, /C:\\>l\s*$/i);
  });

  test("a mapping's tag switches it off and on, its × deletes it", async ({ page }) => {
    await boot(page, { expectScreen: null });
    await openKeyboard(page);
    const mapKey = (c: string) => page.evaluate((k) => (window as any).__test.keyboard.mapKey(k), c);
    await page.evaluate(() => (window as any).__test.keyboard.upsertMapping("KeyN", ["KeyL"], undefined));
    const tag = boardKey(page, "KeyN").locator(".kbd-map-txt");
    await tag.click();
    expect(await mapKey("KeyN")).toEqual(["KeyN"]);
    await expect(boardKey(page, "KeyN")).toHaveClass(/mapped-off/);
    await expect(tag).toHaveAttribute("aria-pressed", "false");
    expect(await page.evaluate(() => (window as any).__test.keyboard.heldKeysSize)).toBe(0);
    await tag.click();
    expect(await mapKey("KeyN")).toEqual(["KeyL"]);
    await expect(boardKey(page, "KeyN")).not.toHaveClass(/mapped-off/);
    await boardKey(page, "KeyN").locator(".kbd-map-del").click();
    expect(await mapKey("KeyN")).toEqual(["KeyN"]);
    await expect(boardKey(page, "KeyN")).not.toHaveClass(/\bmapped\b/);
    expect(await page.evaluate(() => (window as any).__test.keyboard.keymapRows)).toEqual([]);
  });

  test("a switched-off mapping stays off across reload", async ({ page }) => {
    await page.goto("/?test=1&fast=1");
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 60_000 });
    await page.evaluate(() => localStorage.removeItem("retro8080.ibmpcat.keymap"));
    await page.evaluate(() => (window as any).__test.keyboard.upsertMapping("KeyN", ["KeyL"], undefined));
    await openKeyboard(page);
    await boardKey(page, "KeyN").locator(".kbd-map-txt").click();
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 60_000 });
    expect(await page.evaluate(() => (window as any).__test.keyboard.mapKey("KeyN"))).toEqual(["KeyN"]);
    await openKeyboard(page);
    await expect(boardKey(page, "KeyN")).toHaveClass(/mapped-off/);
    await page.evaluate(() => (window as any).__test.keyboard.clearKeymap());
  });

  test("WASD preset, KEYS toggle, and Clear All", async ({ page }) => {
    await boot(page, { expectScreen: null });
    await openKeyboard(page);
    const keys = page.locator("#keymapAllBtn");
    await expect(keys).toBeDisabled();
    await page.locator("#keymapPresetWasd").click();
    await expect(keys).toHaveAttribute("aria-pressed", "true");
    expect(await page.evaluate(() => (window as any).__test.keyboard.mapKey("KeyA"))).toEqual(["AltLeft", "ArrowLeft"]);
    await keys.click();
    expect(await page.evaluate(() => (window as any).__test.keyboard.mapKey("KeyA"))).toEqual(["KeyA"]);
    await page.locator("#keymapClearBtn").click();
    await expect(keys).toBeDisabled();
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 60_000 });
    expect(await page.evaluate(() => (window as any).__test.keyboard.keymapRows)).toEqual([]);
  });
});
