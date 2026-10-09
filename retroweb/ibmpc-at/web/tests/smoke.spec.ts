import { test, expect } from "./fixtures";
import { boot, typeStr, waitForScreen } from "./helpers";

test.describe("boot smoke test", () => {
  test("boots FreeDOS to C:\\> and runs a typed command", async ({ page }) => {
    await boot(page);
    await typeStr(page, "ECHO SMOKE");
    await waitForScreen(page, /C:\\>echo smoke\s*\nsmoke\s*\n/i, 20_000);
  });
});

// Real-speed check. Other specs boot with the fast-test multiplier; this one opts out
// to confirm the wall-clock-paced 8 MHz clock.
test.describe("real-speed smoke test", () => {
  test("the guest CPU runs at real, wall-clock-paced 8 MHz -- not sped up", async ({ page }) => {
    // only needs the machine running, not booted
    await boot(page, { realtime: true, expectScreen: null });

    const c0 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    const t0 = Date.now();
    await page.waitForTimeout(2000);
    const c1 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    const t1 = Date.now();

    const cyclesPerSecond = (c1 - c0) / ((t1 - t0) / 1000);
    // loose bounds for CI jitter, but far below the fast-test rate
    expect(cyclesPerSecond).toBeGreaterThan(4_000_000);
    expect(cyclesPerSecond).toBeLessThan(12_000_000);
  });
});
