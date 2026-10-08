import { test, expect } from "./fixtures";
import { boot, screenText, waitForScreen, focusScreen, clickCtrlAltDel, typeStr, setPowerSwitch, tap } from "./helpers";

// Physical DOM keys -> SET1 scan codes -> 8042, the F-key panel, and Ctrl+Alt+Del.

test.describe("keyboard", () => {
  test("typing through the real focused keyboard reaches COMMAND.COM", async ({ page }) => {
    await boot(page);
    await focusScreen(page);
    await page.keyboard.type("dir", { delay: 40 });
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
    // POST clears the display quickly, proving the combo reached the BIOS
    await expect
      .poll(() => screenText(page), { timeout: 5_000 })
      .not.toMatch(/C:\\>/);
    // then a full reboot back to the prompt
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
    // Print Screen and Pause have fixed multi-byte sequences (Pause has no break code)
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

  test("a held key repeats on the keyboard's own typematic clock", async ({ page }) => {
    await boot(page);
    await page.evaluate(() => (window as any).__test.sendKey("KeyA", false));
    await waitForScreen(page, /C:\\>a{6,}/i, 10_000);
    await page.evaluate(() => (window as any).__test.sendKey("KeyA", true));
    await page.waitForTimeout(200);
    const after = await screenText(page);
    await page.waitForTimeout(500);
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
    await page.waitForTimeout(500);
    expect(await screenText(page)).not.toMatch(/C:\\>b/i);
  });

  test("leaving the screen releases a held key so it stops repeating", async ({ page }) => {
    await boot(page);
    await focusScreen(page);
    await page.keyboard.down("KeyA");
    await waitForScreen(page, /C:\\>a{3,}/i, 10_000);
    await page.locator("#fullscreenBtn").focus();
    await page.waitForTimeout(200);
    const after = await screenText(page);
    await page.waitForTimeout(500);
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
    // clicking any other control blurs the screen -- the hint returns
    await page.locator("#fullscreenBtn").focus();
    expect(await hintVisible()).toBe(true);
    // nothing to type into once powered off -- hidden regardless of focus
    await setPowerSwitch(page, false);
    expect(await hintVisible()).toBe(false);
  });
});
