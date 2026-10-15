import { test, expect } from "./fixtures";
import { screenText, waitForScreen, focusScreen, clickCtrlAltDel, typeStr, setPowerSwitch, tap, openKeyboard, boardKey, keysDrained } from "./helpers";

// Real keyboard path (DOM key events -> SET1 scan codes -> 8042), the F-key panel (a labelled
// substitute for keys a Mac lacks), and the Ctrl+Alt+Del combo. The real-gap timing avoids the
// single-byte 8042 output register clobbering (IBM_PCAT_REVIEW.md §31).

test.describe("keyboard", () => {
  // Holding a movement key while the mouse is captured could leave it stuck: Pointer Lock's release
  // on Escape can skip the keyup. app.js releases all held keys on blur, tab-hide and lock release;
  // this drives that path with a keydown, then blur, and checks the held-key tracking clears.
  test("losing focus while a key is held releases it instead of leaving it stuck", async ({
    livePage: page,
  }) => {
    await focusScreen(page);
    await page.evaluate(() => (window as any).__test.screenEl.dispatchEvent(
      new KeyboardEvent("keydown", { code: "KeyW", bubbles: true })
    ));
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.heldKeysSize))
      .toBe(1);

    // No keyup ever fires, like Pointer Lock swallowing Escape.
    await page.evaluate(() => window.dispatchEvent(new Event("blur")));

    await expect
      .poll(() => page.evaluate(() => (window as any).__test.heldKeysSize))
      .toBe(0);
  });


  test("typing through the real focused keyboard reaches COMMAND.COM", async ({ promptPage: page }) => {
    await focusScreen(page);
    // Lowercase "dir": page.keyboard.type() fires Shift's keydown and the letter's back to back, and
    // keydown/keyup of one key with no gap, which hits the §31 8042 clobbering bug. So per-key
    // down()/up() with a real wait. A human can't release a key in 0ms. DIR finds COMMAND.COM in C:'s root.
    for (const key of ["KeyD", "KeyI", "KeyR"]) {
      await page.keyboard.down(key);
      await keysDrained(page);
      await page.keyboard.up(key);
      await keysDrained(page);
    }
    // Enter needs the same make/break gap; without it Enter's make code was dropped.
    await page.keyboard.down("Enter");
    await keysDrained(page);
    await page.keyboard.up("Enter");
    await waitForScreen(page, /COMMAND/);
    // The prompt returning proves the line was submitted, not just echoed.
    await waitForScreen(page, /C:\\>\s*$/);
  });

  // The keyboard repeats a held key itself (500 ms delay, 10.9 cps); browser repeat events are ignored.
  test("holding a key makes the keyboard repeat it", async ({ promptPage: page }) => {
    await focusScreen(page);
    const c0 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    await page.keyboard.down("KeyX");
    // Typematic runs on guest cycles, so hold for 1.5 s of guest time (66 MHz), not host time.
    await page.waitForFunction((c) => (window as any).__test.machine.totalCycles() - c >= 99_000_000, c0, { timeout: 30_000 });
    await page.keyboard.up("KeyX");
    await expect.poll(async () => (/C:\\>(x+)/.exec(await screenText(page)) || ["", ""])[1].length)
      .toBeGreaterThan(3);
    await tap(page, "Escape");
  });

  test("Ctrl+Alt+Del performs a real warm reboot", async ({ promptPage: page }) => {
    // One FreeDOS wait after CAD on the shared prompt page.
    test.setTimeout(180_000);
    // Plant a marker first: asserting C:\> vanishes races a fast reboot and can't tell a no-op click
    // from a reboot. The marker is gone only if the BIOS took the warm-boot path.
    await focusScreen(page);
    await typeStr(page, "REM CADMARKER", { pressEnterAfter: false });
    await waitForScreen(page, /CADMARKER/i);
    await clickCtrlAltDel(page);
    await expect
      .poll(() => screenText(page), { timeout: 15_000 })
      .not.toMatch(/CADMARKER/i);
    // A full reboot back to the live prompt, not just a blank screen.
    await waitForScreen(page, /C:\\>/, 90_000);
  });

  test("every drawn key sends its Set 1 make and break codes", async ({ livePage: page }) => {
    await openKeyboard(page);
    await page.evaluate(() => {
      const m = (window as any).__test.machine;
      const inject = m.injectScancode.bind(m);
      (window as any).__sent = [];
      (window as any).__inject = inject;
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
    await page.evaluate(() => {
      (window as any).__test.machine.injectScancode = (window as any).__inject;
    });
  });

  test("\"Click to focus\" hint shows only while running and unfocused", async ({ livePage: page }) => {
    const hintVisible = () =>
      page.locator("#focusHint").evaluate((el) => el.classList.contains("visible"));
    // Neither boot() nor app.js's auto power-on focuses the screen, which is the gap the hint covers.
    expect(await hintVisible()).toBe(true);
    await focusScreen(page);
    expect(await hintVisible()).toBe(false);
    // Clicking another control blurs the screen and the hint returns.
    await page.locator("#fullscreenBtn").focus();
    expect(await hintVisible()).toBe(true);
    // Hidden once powered off.
    await setPowerSwitch(page, false);
    expect(await hintVisible()).toBe(false);
  });

  test("\"Barebones FreeDOS\" boot notice shows once, dismisses on focus, and stays dismissed", async ({ livePage: page }) => {
    const noticeVisible = () =>
      page.locator("#bootNotice").evaluate((el) => el.classList.contains("visible"));
    // Re-arm with a power cycle to match a new visitor.
    await setPowerSwitch(page, false);
    await setPowerSwitch(page, true);
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, {
      timeout: 15_000,
    });
    expect(await noticeVisible()).toBe(true);

    // pointer-events: none like .focus-hint: it sits over the screen where a focusing click lands.
    // focusScreen() focuses and (via app.js's focusin listener) dismisses the notice in one gesture.
    await focusScreen(page);
    expect(await noticeVisible()).toBe(false);
    expect(
      await page.evaluate(() => localStorage.getItem("retro8080.pc486BootNoticeDismissed")),
    ).toBe("1");

    // A power cycle does not re-arm it; it stays dismissed in localStorage.
    await setPowerSwitch(page, false);
    await setPowerSwitch(page, true);
    await page.waitForFunction(() => !!(window as any).__test?.machine);
    expect(await noticeVisible()).toBe(false);

    // Clearing localStorage (a new visitor) brings it back on the next power-on.
    await page.evaluate(() => localStorage.clear());
    await setPowerSwitch(page, false);
    await setPowerSwitch(page, true);
    await page.waitForFunction(() => !!(window as any).__test?.machine);
    expect(await noticeVisible()).toBe(true);
  });
});
