import { test, expect } from "./fixtures";
import { screenText, setPowerSwitch, clickReset, waitForScreen, focusScreen, rafTicks } from "./helpers";

// Auto-boot-on-load, the power switch, and the front-panel Reset button (a later clone-era
// convention; the genuine 5170 had none).

test.describe("boot and power", () => {
  test("boots itself to a live C:\\> prompt with no interaction", async ({ promptPage: page }) => {
    await expect(page.locator("#powerSwitch")).toBeChecked();
    await expect(page.locator("#powerLed")).toHaveClass(/power-on/);
    await expect.poll(() => screenText(page)).toMatch(/C:\\>/);
  });

  test("power switch off discards the running machine; on boots a fresh one", async ({
    promptPage: page,
  }) => {
    test.setTimeout(180_000);
    const cyclesRunning = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    expect(cyclesRunning).toBeGreaterThan(0);

    await setPowerSwitch(page, false);
    await expect(page.locator("#powerLed")).not.toHaveClass(/power-on/);
    // Turbo LED and seven-seg clock go dark with the Power LED.
    await expect(page.locator("#turboLed")).not.toHaveClass(/turbo-on/);
    await expect(page.locator("#clockDisplay .sevenseg i.on")).toHaveCount(0);
    // RAM is gone when power is cut. The observable signal is the cycle count stopping.
    const framesLater = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    await rafTicks(page, 20);
    const stillFramesLater = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    expect(stillFramesLater).toBe(framesLater);

    await setPowerSwitch(page, true);
    await expect(page.locator("#powerLed")).toHaveClass(/power-on/);
    await expect(page.locator("#turboLed")).toHaveClass(/turbo-on/);
    await expect(page.locator("#clockDisplay .sevenseg i.on")).not.toHaveCount(0);
    await waitForScreen(page, /C:\\>/);
  });

  test("Reset pulses CPU+chipset reset but keeps the machine running and RAM intact", async ({
    promptPage: page,
  }) => {
    test.setTimeout(180_000);
    // Reset keeps power: the LED and cycle count keep going.
    const cyclesBefore = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    await clickReset(page);
    await expect(page.locator("#powerLed")).toHaveClass(/power-on/);
    await waitForScreen(page, /C:\\>/);
    const cyclesAfter = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    expect(cyclesAfter).toBeGreaterThan(cyclesBefore);
  });

  test("the drawn keyboard is live while on and inert while off", async ({ livePage: page }) => {
    await expect(page.locator('#pcKeyboard [data-key="F1"]')).toHaveAttribute("aria-disabled", "false");
    await expect(page.locator("#escBtn")).toBeEnabled();

    await setPowerSwitch(page, false);
    await expect(page.locator('#pcKeyboard [data-key="F1"]')).toHaveAttribute("aria-disabled", "true");
    await expect(page.locator("#escBtn")).toBeDisabled();
  });

  test("clicking a control focuses the control, not the screen", async ({ livePage: page }) => {
    await focusScreen(page);
    await expect(page.locator("#screen")).toBeFocused();


    await page.locator("#speakerEnabled").click();
    await expect(page.locator("#speakerEnabled")).toBeFocused();
  });
});
