import { test, expect } from "@playwright/test";

test.describe("board controls", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/");
    await expect(page.locator("#screen .xterm-rows")).toContainText("\\", { timeout: 45000 });
  });

  test("power LED is on by default -- D1 is hardwired to +5V in real hardware", async ({ page }) => {
    // D1 is wired to +5V, always lit while the board has power. The Power (J1)
    // control is a UI convenience: the real board has no power switch.
    await expect(page.locator("#pcbLedD1")).toHaveClass(/led-power/);
  });

  test("reset button returns a running session to a fresh Wozmon prompt", async ({ page }) => {
    await page.click("#screen");
    await page.keyboard.type("0.F", { delay: 100 });
    await page.keyboard.press("Enter");
    await expect(page.locator("#screen .xterm-rows")).toContainText("0000:", { timeout: 45000 });

    await page.click('#pcbSvg [data-ref="SW1"]');
    // RESET runs CLEAR_TERMINAL: old output is gone, a fresh "\" is shown
    await expect(page.locator("#screen .xterm-rows")).not.toContainText("0000:", { timeout: 20000 });
    await expect(page.locator("#screen .xterm-rows")).toContainText("\\", { timeout: 45000 });
  });

  test("detaching the LCD reproduces the genuine bare-board hang", async ({ page }) => {
    // Detached, reset_via's busy-poll never clears and the CPU spins in the
    // lcdbusy loop (rom/via.s). Asserted on PC via window.__machine since the
    // scrollback keeps the first boot's banner. C++ twin: machine_test.cpp
    // DetachingTheLcdReproducesTheGenuineBareBoardHang.
    await page.uncheck("#lcdAttached");
    await page.click('#pcbSvg [data-ref="SW1"]');
    await page.waitForTimeout(1500);
    // cycleCount() keeps climbing while pc stays inside the lcdbusy loop body
    const s1 = await page.evaluate(() => ({ pc: window.__machine.state().pc, c: window.__machine.cycleCount() }));
    await page.waitForTimeout(500);
    const s2 = await page.evaluate(() => ({ pc: window.__machine.state().pc, c: window.__machine.cycleCount() }));
    expect(s2.c).toBeGreaterThan(s1.c);
    // range from tmp/firmware.lbl lcd_wait/lcd_instruction; update when code before via.s shifts
    expect(s1.pc).toBeGreaterThanOrEqual(0x80ae);
    expect(s1.pc).toBeLessThanOrEqual(0x80d0);
    expect(s2.pc).toBeGreaterThanOrEqual(0x80ae);
    expect(s2.pc).toBeLessThanOrEqual(0x80d0);
  });

  test("Power (J1) pauses the board in place -- CPU frozen, D1 dimmed, screen content survives", async ({ page }) => {
    // UI convenience, not a hardware fact: pause in place, not reset, so a
    // program mid-edit survives
    await page.click("#screen");
    await page.keyboard.type("0.F", { delay: 100 });
    await page.keyboard.press("Enter");
    await expect(page.locator("#screen .xterm-rows")).toContainText("0000:", { timeout: 45000 });

    await page.click('#pcbSvg [data-ref="J1"]');
    await expect(page.locator("#pcbLedD1")).not.toHaveClass(/led-power/);
    await expect(page.locator("#monitor")).toHaveClass(/powered-off/);

    const before = await page.evaluate(() => window.__machine.cycleCount());
    await page.waitForTimeout(500);
    const after = await page.evaluate(() => window.__machine.cycleCount());
    expect(after).toBe(before);   // genuinely frozen, not just visually dimmed
    await expect(page.locator("#screen .xterm-rows")).toContainText("0000:");   // nothing lost

    await page.click('#pcbSvg [data-ref="J1"]');
    await expect(page.locator("#pcbLedD1")).toHaveClass(/led-power/);
    await expect(page.locator("#monitor")).not.toHaveClass(/powered-off/);
  });

  test("LCD is simply cleared at boot -- no menu banner any more", async ({ page }) => {
    expect(await page.evaluate(() => window.__machine.lcdText())).toBe(" ".repeat(32));
  });
});
