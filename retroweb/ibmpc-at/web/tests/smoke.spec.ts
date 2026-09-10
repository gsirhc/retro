import { test, expect } from "./fixtures";
import { boot } from "./helpers";

// Real-speed timing verification. Every other spec in this suite boots via
// helpers.ts's boot(), which defaults to the fast-test CPU multiplier
// (`?test=1&fast=1` -- see app.js's TEST_CPU_MULTIPLIER) so the suite isn't
// paying a real ~45s 8 MHz POST + FreeDOS boot on every test. This is the
// one test that deliberately opts back out (`realtime: true`), to confirm
// the underlying "genuine, wall-clock-paced 8 MHz" contract (CLAUDE.md's
// "Never speed these up") actually holds -- mirrors altair8800's existing
// "a few tests select Realistic on purpose" convention (its disk/tape
// speed picker), now extended to the CPU clock itself. See CLAUDE.md
// "Current sanctioned overrides" (automated-test CPU clock multiplier).
test.describe("real-speed smoke test", () => {
  test("the guest CPU runs at real, wall-clock-paced 8 MHz -- not sped up", async ({ page }) => {
    // Skip the (slow, at real speed) wait for a live prompt -- this test
    // only needs the machine running, not fully booted.
    await boot(page, { realtime: true, expectScreen: null });

    const c0 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    const t0 = Date.now();
    await page.waitForTimeout(2000);
    const c1 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    const t1 = Date.now();

    const cyclesPerSecond = (c1 - c0) / ((t1 - t0) / 1000);
    // Generous tolerance for CI scheduling jitter (a backgrounded/throttled
    // tab runs frame()'s rAF loop less often, not faster) -- this is
    // checking for genuine ~8 MHz pacing, not tight timing precision, and
    // specifically that it's nowhere near the fast-test multiplier's rate.
    expect(cyclesPerSecond).toBeGreaterThan(4_000_000);
    expect(cyclesPerSecond).toBeLessThan(12_000_000);
  });
});
