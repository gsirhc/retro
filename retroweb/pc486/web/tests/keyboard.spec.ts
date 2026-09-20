import { test, expect } from "./fixtures";
import { boot, screenText, waitForScreen, focusScreen, clickCtrlAltDel, typeStr, setPowerSwitch } from "./helpers";

// The real keyboard path (physical DOM key events -> SET1 scan codes -> the
// emulated 8042), the F-key/extended-key panel (a labelled substitute for
// keys a Mac keyboard has no key for), and the Ctrl+Alt+Del warm-boot combo.
// See IBM_PCAT_REVIEW.md §31 for the single-byte-8042-output-register bug
// this panel's real-gap timing exists to avoid.

test.describe("keyboard", () => {
  // Real bug, reported live: holding a movement key in BOOM while the
  // mouse was captured could leave that key "stuck" pressed forever from
  // the guest's side -- e.g. Pointer Lock's own mandatory release-on-
  // Escape can fire without ever dispatching a keyup (or even a keydown)
  // for Escape to the page at all, so a key held at that moment never got
  // its break code sent. app.js now tracks held keys and releases all of
  // them (synthesizes break codes) on blur, tab-hide, or Pointer Lock
  // release -- this drives that release path directly via a real keydown
  // with no matching keyup, then blur, and checks the held-key tracking
  // (not the DOS-side effect, which needs a specific running program to
  // observe) actually cleared.
  test("losing focus while a key is held releases it instead of leaving it stuck", async ({
    page,
  }) => {
    await boot(page);
    await focusScreen(page);
    await page.evaluate(() => (window as any).__test.screenEl.dispatchEvent(
      new KeyboardEvent("keydown", { code: "KeyW", bubbles: true })
    ));
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.heldKeysSize))
      .toBe(1);

    // No keyup ever fires -- simulates the Pointer-Lock-swallows-Escape
    // case directly via the same browser event a real focus loss fires.
    await page.evaluate(() => window.dispatchEvent(new Event("blur")));

    await expect
      .poll(() => page.evaluate(() => (window as any).__test.heldKeysSize))
      .toBe(0);
  });


  test("typing through the real focused keyboard reaches COMMAND.COM", async ({ page }) => {
    await boot(page);
    await focusScreen(page);
    // Lowercase, not "DIR" -- DOS is case-insensitive so this is still a
    // real, faithful command, and it sidesteps a genuine finding from this
    // test: page.keyboard.type()'s uppercase-letter path fires Shift's own
    // keydown and the letter's keydown back to back with no real gap
    // between them, which hits the exact single-byte-8042-output-register
    // clobbering bug IBM_PCAT_REVIEW.md §31 documents for the Ctrl+Alt+Del
    // combo -- Shift's make code loses the race and the guest never sees
    // it held, so every "uppercase" letter arrives lowercase anyway.
    //
    // Per-key down()/up() with a real wait, not page.keyboard.type(): a
    // second, deeper finding from this test is that type() fires a single
    // key's own keydown and keyup back to back with no real gap either
    // (its `delay` option only paces *between* keys, not within one),
    // which hits the exact same clobbering bug for every letter, not just
    // Shift -- confirmed by reproducing it with type() at any delay and
    // fixing it with the explicit per-key gap below instead. A real human
    // can't physically release a key in 0ms, so this never happens outside
    // synthetic test events; the gap here just makes the test honest about
    // that rather than relying on an API that doesn't provide it. DIR
    // always finds COMMAND.COM in C:'s root (see IBM_PCAT_REVIEW.md's
    // "genuine FreeDOS 1.3 kernel" boot confirmation).
    for (const key of ["KeyD", "KeyI", "KeyR"]) {
      await page.keyboard.down(key);
      await page.waitForTimeout(60);
      await page.keyboard.up(key);
      await page.waitForTimeout(60);
    }
    // Enter needs the same real make/break gap, for the same reason -- it
    // silently dropped Enter's own make code in exactly this test (DIR's
    // line sat typed but never submitted, even after 2 real minutes)
    // until this was split into down()/up() with a real wait.
    await page.keyboard.down("Enter");
    await page.waitForTimeout(60);
    await page.keyboard.up("Enter");
    await waitForScreen(page, /COMMAND/);
    // the prompt returns once DIR finishes, proving the line was actually
    // submitted and processed, not just echoed
    await waitForScreen(page, /C:\\>\s*$/);
  });

  test("Ctrl+Alt+Del performs a real warm reboot", async ({ page }) => {
    await boot(page);
    await clickCtrlAltDel(page);
    // POST clears and re-initializes the display almost immediately in
    // machine time -- the screen should leave the old prompt well within a
    // few real seconds, proving the combo actually reached the BIOS's
    // keyboard ISR (the exact thing §31's bug silently failed to do: only
    // Del ever arrived, with no Ctrl/Alt held, so the BIOS never recognized
    // it and nothing happened at all).
    await expect
      .poll(() => screenText(page), { timeout: 5_000 })
      .not.toMatch(/C:\\>/);
    // ...and the machine finishes a genuine full reboot back to the same
    // live prompt, not just a blanked screen.
    await waitForScreen(page, /C:\\>/, 120_000);
  });

  test("function-key panel (F1-F12) stays live and doesn't desync the keyboard", async ({ page }) => {
    await boot(page);
    for (const key of ["F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"]) {
      await page.locator(`[data-key="${key}"]`).click();
    }
    const cycles1 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    await page.waitForTimeout(300);
    const cycles2 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    expect(cycles2).toBeGreaterThan(cycles1); // still running, not hung

    // and ordinary typing still reaches COMMAND.COM afterward
    await focusScreen(page);
    await typeStr(page, "VER");
    await waitForScreen(page, /C:\\>\s*$/);
  });

  test("extended-key panel (Insert/Delete/Home/End/PgUp/PgDn/PrintScreen/ScrollLock/Pause/NumLock) stays live", async ({ page }) => {
    await boot(page);
    // Print Screen and Pause/Break are the two keys with non-standard,
    // fixed multi-byte sequences (Pause has no break code at all -- a
    // genuine AT keyboard quirk, see app.js's SET1 table comment) --
    // exactly the sequences most likely to desync the 8042 if a future
    // change reintroduces the §31 clobbering bug for them specifically.
    for (const key of [
      "Insert", "Delete", "Home", "End", "PageUp", "PageDown",
      "PrintScreen", "ScrollLock", "Pause", "NumLock",
    ]) {
      await page.locator(`[data-key="${key}"]`).click();
    }
    // the keyboard must still be fully responsive after all of them
    await focusScreen(page);
    await typeStr(page, "VER");
    await waitForScreen(page, /C:\\>\s*$/);
  });

  test("\"Click to focus\" hint shows only while running and unfocused", async ({ page }) => {
    await boot(page);
    const hintVisible = () =>
      page.locator("#focusHint").evaluate((el) => el.classList.contains("visible"));
    // boot() never focuses the screen itself -- neither does app.js's own
    // auto power-on at a fresh page load -- which is exactly the gap this
    // hint exists to cover, so it should already be showing.
    expect(await hintVisible()).toBe(true);
    await focusScreen(page);
    expect(await hintVisible()).toBe(false);
    // clicking any other control blurs the screen -- the hint returns
    await page.locator("#fullscreenBtn").focus();
    expect(await hintVisible()).toBe(true);
    // nothing to type into once powered off -- hidden regardless of focus
    await setPowerSwitch(page, false);
    expect(await hintVisible()).toBe(false);
  });
});
