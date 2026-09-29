import { test, expect } from "./fixtures";

// The front panel's turbo cluster: seven-segment clock readout tracks
// Turbo (66 / 33), amber LED matches, and Turbo actually changes the
// guest clock (DX2 doubling). Power/reset behavior is in boot.spec.ts.

test.describe("front panel jewelry", () => {
  test("the seven-segment display shows 66 with Turbo on, 33 with Turbo off", async ({ livePage: page }) => {
    const digits = page.locator(".sevenseg");
    await expect(digits).toHaveCount(2);

    // Turbo on (default): both digits are "6" (everything but b).
    for (let i = 0; i < 2; i++) {
      const digit = digits.nth(i);
      for (const seg of ["a", "c", "d", "e", "f", "g"]) {
        await expect(digit.locator(`.${seg}`)).toHaveClass(/on/);
      }
      await expect(digit.locator(".b")).not.toHaveClass(/on/);
    }

    await page.locator("#turboBtn").click();
    // Turbo off: both digits are "3" (a/b/c/d/g; not e/f).
    for (let i = 0; i < 2; i++) {
      const digit = digits.nth(i);
      for (const seg of ["a", "b", "c", "d", "g"]) {
        await expect(digit.locator(`.${seg}`)).toHaveClass(/on/);
      }
      await expect(digit.locator(".e")).not.toHaveClass(/on/);
      await expect(digit.locator(".f")).not.toHaveClass(/on/);
    }

    await page.locator("#turboBtn").click();
    await expect(digits.nth(0).locator(".e")).toHaveClass(/on/);  // back to "6"
  });

  test("reads as a tower turbo cluster with 5.25\" CD above 3.5\" floppy", async ({ livePage: page }) => {
    await expect(page.locator(".tower-panel")).toBeVisible();
    await expect(page.locator("#turboBtn")).toBeVisible();
    await expect(page.locator("#resetBtn")).toBeVisible();
    await expect(page.locator("#powerSwitch")).toBeVisible();
    await expect(page.locator(".power-rocker")).toBeVisible();
    await expect(page.locator(".tower-keylock")).toHaveCount(0);

    // Usual tower stack: 5.25" CD-ROM on top, 3.5" floppy below.
    const drives = page.locator(".at-drives .at-bay");
    await expect(drives).toHaveCount(2);
    await expect(drives.nth(0)).toHaveAttribute("data-drive", "cdrom");
    await expect(drives.nth(0)).toHaveClass(/bay-525/);
    await expect(drives.nth(0).locator(".cd-door")).toBeVisible();
    await expect(drives.nth(1)).toHaveAttribute("data-drive", "0");
    await expect(drives.nth(1)).toHaveClass(/bay-35/);
    await expect(drives.nth(1).locator(".floppy-door")).toBeVisible();
  });

  test("Turbo toggles DX2 clock doubling: 66 MHz on, 33 MHz off", async ({ livePage: page }) => {
    const btn = page.locator("#turboBtn");
    const led = page.locator("#turboLed");
    await expect(btn).toHaveAttribute("aria-pressed", "true");
    await expect(led).toHaveClass(/turbo-on/);

    // Turbo on: ~66M cycles/sec. Measure over a short wall window.
    const rateOn = await page.evaluate(async () => {
      const m = (window as any).__test.machine;
      const t0 = performance.now();
      const c0 = m.totalCycles();
      await new Promise((r) => setTimeout(r, 200));
      return (m.totalCycles() - c0) / ((performance.now() - t0) / 1000);
    });
    // Absolute floor is soft: CI hosts often sustain only ~30-36 MHz of the
    // intended 66, and the wall-clock chunk drop in app.js then caps the
    // measured rate. Ratio vs Turbo-off below is the real DX2 check.
    expect(rateOn).toBeGreaterThan(25e6);

    await btn.click();
    await expect(btn).toHaveAttribute("aria-pressed", "false");
    await expect(led).not.toHaveClass(/turbo-on/);
    await expect.poll(async () => page.evaluate(() => (window as any).__test.machine.cpuHz())).toBe(33000000);

    const rateOff = await page.evaluate(async () => {
      const m = (window as any).__test.machine;
      const t0 = performance.now();
      const c0 = m.totalCycles();
      await new Promise((r) => setTimeout(r, 200));
      return (m.totalCycles() - c0) / ((performance.now() - t0) / 1000);
    });
    // Off should be roughly half of on (same host, same load).
    expect(rateOff).toBeLessThan(rateOn * 0.7);
    expect(rateOff).toBeGreaterThan(15e6);

    await btn.click();
    await expect(btn).toHaveAttribute("aria-pressed", "true");
    await expect(led).toHaveClass(/turbo-on/);
    await expect.poll(async () => page.evaluate(() => (window as any).__test.machine.cpuHz())).toBe(66000000);
  });
});
