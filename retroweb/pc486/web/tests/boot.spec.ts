import { test, expect } from "./fixtures";
import { boot, screenText, setPowerSwitch, clickReset, waitForScreen, focusScreen } from "./helpers";

// Auto-boot-on-load, the power switch, and the front-panel Reset button
// (a real, later clone-era convention this machine's whole premise calls
// for -- unlike ibmpc-at's genuine 5170, which never had one).

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
    // Real hardware: RAM is gone the instant power is cut -- see
    // ibmpc-at/web/tests/boot.spec.ts's identical test for the full
    // rationale (the observable signal is the cycle count stopping, not
    // window.__test.machine becoming null).
    const framesLater = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    await page.waitForTimeout(300);
    const stillFramesLater = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    expect(stillFramesLater).toBe(framesLater);

    await setPowerSwitch(page, true);
    await expect(page.locator("#powerLed")).toHaveClass(/power-on/);
    await waitForScreen(page, /C:\\>/);
  });

  test("Reset pulses CPU+chipset reset but keeps the machine running and RAM intact", async ({ page }) => {
    await boot(page);
    // A real reset button doesn't cut power -- the power LED and cycle
    // count both keep going, unlike the power-off case above.
    const cyclesBefore = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    await clickReset(page);
    await expect(page.locator("#powerLed")).toHaveClass(/power-on/);
    // A fresh boot follows the reset, back to the same live prompt.
    await waitForScreen(page, /C:\\>/);
    const cyclesAfter = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    expect(cyclesAfter).toBeGreaterThan(cyclesBefore);
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
    await boot(page);
    await focusScreen(page);
    await expect(page.locator("#screen")).toBeFocused();

    await page.locator('[data-key="F5"]').click();
    await expect(page.locator('[data-key="F5"]')).toBeFocused();

    await page.locator("#speakerEnabled").click();
    await expect(page.locator("#speakerEnabled")).toBeFocused();
  });
});
