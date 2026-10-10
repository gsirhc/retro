import { test, expect } from "./fixtures";
import { screenText, waitForScreen, focusScreen, clickCtrlAltDel, typeStr, setPowerSwitch, tap } from "./helpers";

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
      await page.waitForTimeout(60);
      await page.keyboard.up(key);
      await page.waitForTimeout(60);
    }
    // Enter needs the same make/break gap; without it Enter's make code was dropped.
    await page.keyboard.down("Enter");
    await page.waitForTimeout(60);
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

  test("function-key panel (F1-F12) stays live and doesn't desync the keyboard", async ({ promptPage: page }) => {
    for (const key of ["F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"]) {
      await page.locator(`[data-key="${key}"]`).click();
    }
    const cycles1 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    await page.waitForTimeout(300);
    const cycles2 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    expect(cycles2).toBeGreaterThan(cycles1);

    // Ordinary typing still reaches COMMAND.COM.
    await focusScreen(page);
    await typeStr(page, "VER");
    await waitForScreen(page, /C:\\>\s*$/);
  });

  test("extended-key panel (Insert/Delete/Home/End/PgUp/PgDn/PrintScreen/ScrollLock/Pause/NumLock) stays live", async ({ promptPage: page }) => {
    // Print Screen and Pause/Break have fixed multi-byte sequences (Pause has no break code, an AT
    // quirk, see app.js SET1 table) and are the likeliest to desync the 8042.
    for (const key of [
      "Insert", "Delete", "Home", "End", "PageUp", "PageDown",
      "PrintScreen", "ScrollLock", "Pause", "NumLock",
    ]) {
      await page.locator(`[data-key="${key}"]`).click();
    }
    // The keyboard stays responsive after all of them.
    await focusScreen(page);
    await typeStr(page, "VER");
    await waitForScreen(page, /C:\\>\s*$/);
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
    expect(await noticeVisible()).toBe(false);

    // Clearing localStorage (a new visitor) brings it back on the next power-on.
    await page.evaluate(() => localStorage.clear());
    await setPowerSwitch(page, false);
    await setPowerSwitch(page, true);
    expect(await noticeVisible()).toBe(true);
  });
});
