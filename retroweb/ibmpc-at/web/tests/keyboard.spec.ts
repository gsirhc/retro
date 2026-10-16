import { test, expect } from "./fixtures";
import { boot, screenText, waitForScreen, focusScreen, clickCtrlAltDel, typeStr, setPowerSwitch, tap, openKeyboard, boardKey, emuWait } from "./helpers";

// Physical DOM keys -> SET1 scan codes -> 8042, the drawn keyboard, and Ctrl+Alt+Del.

test.describe("keyboard", () => {
  test("typing through the real focused keyboard reaches COMMAND.COM", async ({ page }) => {
    await boot(page);
    await focusScreen(page);
    await page.keyboard.type("dir", { delay: 40 });
    await page.keyboard.down("Enter");
    await emuWait(page, 60);
    await page.keyboard.up("Enter");
    await waitForScreen(page, /COMMAND/);
    // the prompt returns once DIR finishes, proving the line was actually
    // submitted and processed, not just echoed
    await waitForScreen(page, /C:\\>\s*$/);
  });

  test("Ctrl+Alt+Del performs a real warm reboot", async ({ page }) => {
    await boot(page);
    await clickCtrlAltDel(page);
    // POST clears the display quickly, proving the combo reached the BIOS
    await expect
      .poll(() => screenText(page), { timeout: 5_000 })
      .not.toMatch(/C:\\>/);
    // then a full reboot back to the prompt
    await waitForScreen(page, /C:\\>/, 120_000);
  });

  test("every drawn key sends its Set 1 make and break codes", async ({ page }) => {
    await boot(page);
    await openKeyboard(page);
    await page.evaluate(() => {
      const m = (window as any).__test.machine;
      const inject = m.injectScancode.bind(m);
      (window as any).__sent = [];
      m.injectScancode = (code: number) => {
        (window as any).__sent.push(code);
        inject(code);
      };
    });
    // Make codes, IBM Enhanced Keyboard in the 8042's translated Set 1.
    const plain: Record<string, number> = {
      Escape: 0x01, Digit1: 0x02, Digit2: 0x03, Digit3: 0x04, Digit4: 0x05, Digit5: 0x06, Digit6: 0x07,
      Digit7: 0x08, Digit8: 0x09, Digit9: 0x0a, Digit0: 0x0b, Minus: 0x0c, Equal: 0x0d, Backspace: 0x0e,
      Tab: 0x0f, KeyQ: 0x10, KeyW: 0x11, KeyE: 0x12, KeyR: 0x13, KeyT: 0x14, KeyY: 0x15, KeyU: 0x16,
      KeyI: 0x17, KeyO: 0x18, KeyP: 0x19, BracketLeft: 0x1a, BracketRight: 0x1b, Enter: 0x1c,
      KeyA: 0x1e, KeyS: 0x1f, KeyD: 0x20, KeyF: 0x21, KeyG: 0x22, KeyH: 0x23, KeyJ: 0x24, KeyK: 0x25,
      KeyL: 0x26, Semicolon: 0x27, Quote: 0x28, Backquote: 0x29, Backslash: 0x2b,
      KeyZ: 0x2c, KeyX: 0x2d, KeyC: 0x2e, KeyV: 0x2f, KeyB: 0x30, KeyN: 0x31, KeyM: 0x32,
      Comma: 0x33, Period: 0x34, Slash: 0x35, NumpadMultiply: 0x37, Space: 0x39, CapsLock: 0x3a,
      F1: 0x3b, F2: 0x3c, F3: 0x3d, F4: 0x3e, F5: 0x3f, F6: 0x40, F7: 0x41, F8: 0x42, F9: 0x43, F10: 0x44,
      NumLock: 0x45, ScrollLock: 0x46, Numpad7: 0x47, Numpad8: 0x48, Numpad9: 0x49, NumpadSubtract: 0x4a,
      Numpad4: 0x4b, Numpad5: 0x4c, Numpad6: 0x4d, NumpadAdd: 0x4e, Numpad1: 0x4f, Numpad2: 0x50,
      Numpad3: 0x51, Numpad0: 0x52, NumpadDecimal: 0x53, F11: 0x57, F12: 0x58,
    };
    const extended: Record<string, number> = {
      Insert: 0x52, Delete: 0x53, Home: 0x47, End: 0x4f, PageUp: 0x49, PageDown: 0x51,
      ArrowUp: 0x48, ArrowLeft: 0x4b, ArrowRight: 0x4d, ArrowDown: 0x50,
      NumpadEnter: 0x1c, NumpadDivide: 0x35,
    };
    const expected: Record<string, number[]> = {
      PrintScreen: [0xe0, 0x2a, 0xe0, 0x37, 0xe0, 0xb7, 0xe0, 0xaa],
      Pause: [0xe1, 0x1d, 0x45, 0xe1, 0x9d, 0xc5],
    };
    for (const [k, c] of Object.entries(plain)) expected[k] = [c, c | 0x80];
    for (const [k, c] of Object.entries(extended)) expected[k] = [0xe0, c, 0xe0, c | 0x80];
    // Modifiers latch: make on the first click, break on the second.
    const mods: Record<string, number[]> = {
      ShiftLeft: [0x2a, 0xaa], ShiftRight: [0x36, 0xb6], ControlLeft: [0x1d, 0x9d],
      AltLeft: [0x38, 0xb8], ControlRight: [0xe0, 0x1d, 0xe0, 0x9d], AltRight: [0xe0, 0x38, 0xe0, 0xb8],
    };

    const drawn = await page.locator("#pcKeyboard [data-key]").evaluateAll((els) =>
      els.map((e) => (e as HTMLElement).dataset.key!),
    );
    expect(drawn).toHaveLength(101);
    expect(drawn.slice().sort()).toEqual([...Object.keys(expected), ...Object.keys(mods)].sort());
    for (const [key, bytes] of Object.entries({ ...expected, ...mods })) {
      await page.evaluate(() => { (window as any).__sent = []; });
      await boardKey(page, key).click();
      if (key in mods) await boardKey(page, key).click();
      await expect
        .poll(() => page.evaluate(() => (window as any).__sent), { message: key })
        .toEqual(bytes);
    }
  });

  test("F3 and Home/End from the drawn keyboard reach COMMAND.COM's line editor", async ({ page }) => {
    await boot(page);
    await openKeyboard(page);
    await typeStr(page, "VER");
    await waitForScreen(page, /C:\\>\s*$/);
    await boardKey(page, "F3").click();
    await waitForScreen(page, /C:\\>ver\s*$/i);
    await tap(page, "Escape");

    await typeStr(page, "ABC", { pressEnterAfter: false });
    await boardKey(page, "Home").click();
    await typeStr(page, "X", { pressEnterAfter: false });
    await boardKey(page, "End").click();
    await typeStr(page, "Y", { pressEnterAfter: false });
    await waitForScreen(page, /C:\\>xabcy\s*$/i);
  });

  test("a held key repeats on the keyboard's own typematic clock", async ({ page }) => {
    await boot(page);
    await page.evaluate(() => (window as any).__test.sendKey("KeyA", false));
    await waitForScreen(page, /C:\\>a{6,}/i, 10_000);
    await page.evaluate(() => (window as any).__test.sendKey("KeyA", true));
    await emuWait(page, 200);
    const after = await screenText(page);
    await emuWait(page, 500);
    expect(await screenText(page)).toBe(after);
    await tap(page, "Escape");
  });

  test("browser auto-repeat keydowns don't reach the guest", async ({ page }) => {
    await boot(page);
    await focusScreen(page);
    await page.locator("#screen").evaluate((el) => {
      for (let i = 0; i < 5; i++) {
        el.dispatchEvent(new KeyboardEvent("keydown", { code: "KeyB", repeat: true, bubbles: true }));
      }
    });
    await emuWait(page, 500);
    expect(await screenText(page)).not.toMatch(/C:\\>b/i);
  });

  test("leaving the screen releases a held key so it stops repeating", async ({ page }) => {
    await boot(page);
    await focusScreen(page);
    await page.keyboard.down("KeyA");
    await waitForScreen(page, /C:\\>a{3,}/i, 10_000);
    await page.locator("#fullscreenBtn").focus();
    await emuWait(page, 200);
    const after = await screenText(page);
    await emuWait(page, 500);
    expect(await screenText(page)).toBe(after);
    await page.keyboard.up("KeyA");
  });

  test("\"Click to focus\" hint shows only while running and unfocused", async ({ page }) => {
    await boot(page);
    const hintVisible = () =>
      page.locator("#focusHint").evaluate((el) => el.classList.contains("visible"));
    // boot() never focuses the screen, so the hint should already show
    expect(await hintVisible()).toBe(true);
    await focusScreen(page);
    expect(await hintVisible()).toBe(false);
    // clicking any other control blurs the screen, the hint returns
    await page.locator("#fullscreenBtn").focus();
    expect(await hintVisible()).toBe(true);
    // nothing to type into once powered off, hidden regardless of focus
    await setPowerSwitch(page, false);
    expect(await hintVisible()).toBe(false);
  });
});
