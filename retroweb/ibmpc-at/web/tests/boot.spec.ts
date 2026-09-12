import { test, expect } from "./fixtures";
import { boot, screenText, setPowerSwitch, waitForScreen, focusScreen } from "./helpers";

// Auto-boot-on-load and the power switch. A real AT boots the instant it's
// switched on; this page goes one step further (a labelled UI choice, not a
// hardware fact -- see app.js's firmware-fetch comment) and flips the switch
// itself the moment firmware is ready, so a visitor lands on a running
// machine without hunting for a control first.

test.describe("boot and power", () => {
  test("boots itself to a live C:\\> prompt with no interaction", async ({ page }) => {
    await boot(page);
    await expect(page.locator("#powerSwitch")).toBeChecked();
    await expect(page.locator("#powerLed")).toHaveClass(/power-on/);
    await expect.poll(() => screenText(page)).toMatch(/C:\\>/);
  });

  test("power switch off discards the running machine; on boots a fresh one", async ({ page }) => {
    await boot(page);
    const cyclesRunning = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    expect(cyclesRunning).toBeGreaterThan(0);

    await setPowerSwitch(page, false);
    await expect(page.locator("#powerLed")).not.toHaveClass(/power-on/);
    // Real hardware: RAM is gone the instant power is cut -- app.js's
    // powerOff() sets its own `machine` closure variable to null and stops
    // frame() from rescheduling itself. window.__test is only ever
    // (re)assigned inside powerOn() (see app.js), so window.__test.machine
    // itself still references the old, now-inert instance rather than
    // becoming null -- the real, observable signal that the CPU actually
    // stopped is that its cycle count stops advancing, not that this
    // test-only reference gets nulled out.
    const framesLater = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    await page.waitForTimeout(300);
    const stillFramesLater = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    expect(stillFramesLater).toBe(framesLater);

    await setPowerSwitch(page, true);
    await expect(page.locator("#powerLed")).toHaveClass(/power-on/);
    await waitForScreen(page, /C:\\>/);
  });

  test("F-keys and Ctrl+Alt+Del are disabled while powered off, enabled while on", async ({ page }) => {
    await boot(page);
    await expect(page.locator("#ctrlAltDelBtn")).toBeEnabled();
    await expect(page.locator('[data-key="F1"]')).toBeEnabled();

    await setPowerSwitch(page, false);
    await expect(page.locator("#ctrlAltDelBtn")).toBeDisabled();
    await expect(page.locator('[data-key="F1"]')).toBeDisabled();
  });

  test("clicking a control focuses the control, not the screen", async ({ page }) => {
    // A prior version of app.js refocused #screen after every click
    // anywhere on the page (see IBM_PCAT_REVIEW.md §33), meant to route the
    // visitor's very next keystroke to the guest even right after clicking
    // a button. In practice that made every other control effectively
    // unusable via the keyboard the instant you clicked it -- e.g. a
    // checkbox couldn't be toggled with Space right after clicking it,
    // since focus had already bounced back to the canvas. Only clicking the
    // screen itself should focus the screen; other controls keep normal
    // browser focus behavior. See IBM_PCAT_REVIEW.md's follow-up note.
    await boot(page);
    await focusScreen(page);
    await expect(page.locator("#screen")).toBeFocused();

    await page.locator('[data-key="F5"]').click();
    await expect(page.locator('[data-key="F5"]')).toBeFocused();

    await page.locator("#speakerEnabled").click();
    await expect(page.locator("#speakerEnabled")).toBeFocused();
  });
});
