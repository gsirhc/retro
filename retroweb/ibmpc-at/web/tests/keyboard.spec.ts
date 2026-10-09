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

  test("every key button sends its Set 1 make and break codes", async ({ page }) => {
    await boot(page);
    await page.evaluate(() => {
      const m = (window as any).__test.machine;
      const inject = m.injectScancode.bind(m);
      (window as any).__sent = [];
      m.injectScancode = (code: number) => {
        (window as any).__sent.push(code);
        inject(code);
      };
    });
    const ext = (code: number) => [0xe0, code, 0xe0, code | 0x80];
    const expected: Record<string, number[]> = {
      F1: [0x3b, 0xbb], F2: [0x3c, 0xbc], F3: [0x3d, 0xbd], F4: [0x3e, 0xbe],
      F5: [0x3f, 0xbf], F6: [0x40, 0xc0], F7: [0x41, 0xc1], F8: [0x42, 0xc2],
      F9: [0x43, 0xc3], F10: [0x44, 0xc4], F11: [0x57, 0xd7], F12: [0x58, 0xd8],
      Insert: ext(0x52), Delete: ext(0x53), Home: ext(0x47), End: ext(0x4f),
      PageUp: ext(0x49), PageDown: ext(0x51),
      PrintScreen: [0xe0, 0x2a, 0xe0, 0x37, 0xe0, 0xb7, 0xe0, 0xaa],
      ScrollLock: [0x46, 0xc6],
      Pause: [0xe1, 0x1d, 0x45, 0xe1, 0x9d, 0xc5],
      NumLock: [0x45, 0xc5],
    };
    const buttons = await page.locator("#fkeyRow [data-key], #extraKeyRow [data-key]").evaluateAll((els) =>
      els.map((e) => (e as HTMLElement).dataset.key!),
    );
    expect(buttons.sort()).toEqual(Object.keys(expected).sort());
    for (const [key, bytes] of Object.entries(expected)) {
      await page.evaluate(() => { (window as any).__sent = []; });
      await page.locator(`[data-key="${key}"]`).click();
      await expect
        .poll(() => page.evaluate(() => (window as any).__sent), { message: key })
        .toEqual(bytes);
    }
  });

  test("F3 and Home/End from the button rows reach COMMAND.COM's line editor", async ({ page }) => {
    await boot(page);
    await typeStr(page, "VER");
    await waitForScreen(page, /C:\\>\s*$/);
    await page.locator('[data-key="F3"]').click();
    await waitForScreen(page, /C:\\>ver\s*$/i);
    await tap(page, "Escape");

    await typeStr(page, "ABC", { pressEnterAfter: false });
    await page.locator('[data-key="Home"]').click();
    await typeStr(page, "X", { pressEnterAfter: false });
    await page.locator('[data-key="End"]').click();
    await typeStr(page, "Y", { pressEnterAfter: false });
    await waitForScreen(page, /C:\\>xabcy\s*$/i);
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
