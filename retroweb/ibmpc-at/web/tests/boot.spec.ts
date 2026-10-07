import { test, expect } from "./fixtures";
import { boot, screenText, setPowerSwitch, waitForScreen, focusScreen } from "./helpers";

// Auto-boot on load and the power switch.

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
    // window.__test.machine still points at the old instance, so check that the cycle count stops.
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
    // Only clicking the screen focuses it; other controls keep normal focus.
    await boot(page);
    await focusScreen(page);
    await expect(page.locator("#screen")).toBeFocused();

    await page.locator('[data-key="F5"]').click();
    await expect(page.locator('[data-key="F5"]')).toBeFocused();

    await page.locator("#speakerEnabled").click();
    await expect(page.locator("#speakerEnabled")).toBeFocused();
  });
});
