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

  test("clicking any control refocuses the screen, not the control", async ({ page }) => {
    // Every click on the page steals focus onto whatever was clicked (the
    // ordinary browser default) unless something puts it back -- app.js's
    // page-wide click listener refocuses #screen after every click once a
    // machine is running, so the visitor's very next keystroke still goes
    // to the guest instead of being silently swallowed by a button. See
    // IBM_PCAT_REVIEW.md §33.
    await boot(page);
    await focusScreen(page);
    await expect(page.locator("#screen")).toBeFocused();

    await page.locator('[data-key="F5"]').click();
    await expect(page.locator("#screen")).toBeFocused();

    await page.locator("#speakerEnabled").click();
    await expect(page.locator("#screen")).toBeFocused();
  });
});
