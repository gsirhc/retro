import { test, expect } from "@playwright/test";

test.describe("board controls", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/");
    await expect(page.locator("#screen")).toContainText("\\", { timeout: 25000 });
  });

  test("power LED is on by default -- D1 is hardwired to +5V in real hardware", async ({ page }) => {
    // Real hardware: D1 is wired straight to +5V, no software involvement
    // (see machine.cpp's on_power_led) -- always lit whenever the board
    // has power. The Power (J1) control below is a labelled UI convenience
    // with no real-hardware equivalent (the real board has no power
    // switch -- J1's just a barrel jack), so it's the one thing here that
    // overrides D1's class; see the next test for its "off" side.
    await expect(page.locator("#pcbLedD1")).toHaveClass(/led-power/);
  });

  test("reset button returns a running session to a fresh Wozmon prompt", async ({ page }) => {
    await page.click("#screen");
    await page.keyboard.type("0.F", { delay: 100 });
    await page.keyboard.press("Enter");
    await expect(page.locator("#screen")).toContainText("0000:", { timeout: 25000 });

    await page.click('#pcbSvg [data-ref="SW1"]');
    // RESET runs CLEAR_TERMINAL again -- the screen is wiped, not appended
    // to, so the old examine output should be gone and a fresh "\" shown.
    await expect(page.locator("#screen")).not.toContainText("0000:", { timeout: 5000 });
    await expect(page.locator("#screen")).toContainText("\\", { timeout: 25000 });
  });

  test("detaching the LCD reproduces the genuine bare-board hang", async ({ page }) => {
    // Detach, then reset -- reset_via's busy-poll should never clear, so
    // the CPU spins forever at the lcdbusy loop (rom/via.s) and never
    // reaches Wozmon's boot banner again. Asserted directly against the
    // CPU's own PC (exposed via window.__machine, see app.js) rather than
    // terminal text, for the same reason as before: the page never clears
    // its scrollback, so the *first* successful boot's banner legitimately
    // stays on screen regardless. Same scenario, C++-level, in
    // tests/machine_test.cpp's DetachingTheLcdReproducesTheGenuineBareBoardHang.
    await page.uncheck("#lcdAttached");
    await page.click('#pcbSvg [data-ref="SW1"]');
    await page.waitForTimeout(1500);
    // Two samples a moment apart: cycleCount() (real elapsed CPU cycles)
    // must keep climbing -- it's still running, not crashed -- while pc
    // stays within the tight lcdbusy loop body (rom/via.s) rather than
    // exact-matching one instruction's address, since where exactly in
    // that loop it sits when sampled isn't itself meaningful.
    const s1 = await page.evaluate(() => ({ pc: window.__machine.state().pc, c: window.__machine.cycleCount() }));
    await page.waitForTimeout(500);
    const s2 = await page.evaluate(() => ({ pc: window.__machine.state().pc, c: window.__machine.cycleCount() }));
    expect(s2.c).toBeGreaterThan(s1.c);
    // Range re-verified against tmp/firmware.lbl's lcd_wait/lcd_instruction
    // labels after each ROM change that shifts code before via.s.
    expect(s1.pc).toBeGreaterThanOrEqual(0x80ae);
    expect(s1.pc).toBeLessThanOrEqual(0x80d0);
    expect(s2.pc).toBeGreaterThanOrEqual(0x80ae);
    expect(s2.pc).toBeLessThanOrEqual(0x80d0);
  });

  test("Power (J1) pauses the board in place -- CPU frozen, D1 dimmed, screen content survives", async ({ page }) => {
    // A labelled UI convenience (see app.js's comment by the click handler
    // and the previous test) -- the real board has no power switch to
    // model, so this behavior is this page's own design choice, not a
    // hardware fact: pause in place, not reset, so a program mid-edit
    // survives the "power cycle".
    await page.click("#screen");
    await page.keyboard.type("0.F", { delay: 100 });
    await page.keyboard.press("Enter");
    await expect(page.locator("#screen")).toContainText("0000:", { timeout: 25000 });

    await page.click('#pcbSvg [data-ref="J1"]');
    await expect(page.locator("#pcbLedD1")).not.toHaveClass(/led-power/);
    await expect(page.locator("#monitor")).toHaveClass(/powered-off/);

    const before = await page.evaluate(() => window.__machine.cycleCount());
    await page.waitForTimeout(500);
    const after = await page.evaluate(() => window.__machine.cycleCount());
    expect(after).toBe(before);   // genuinely frozen, not just visually dimmed
    await expect(page.locator("#screen")).toContainText("0000:");   // nothing lost

    await page.click('#pcbSvg [data-ref="J1"]');
    await expect(page.locator("#pcbLedD1")).toHaveClass(/led-power/);
    await expect(page.locator("#monitor")).not.toHaveClass(/powered-off/);
  });

  test("LCD is simply cleared at boot -- no menu banner any more", async ({ page }) => {
    expect(await page.evaluate(() => window.__machine.lcdText())).toBe(" ".repeat(32));
  });
});
