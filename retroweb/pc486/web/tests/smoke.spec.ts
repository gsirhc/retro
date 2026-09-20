import { test, expect } from "./fixtures";
import { boot } from "./helpers";

// Real-speed timing verification. Every other spec in this suite boots via
// helpers.ts's boot(), which defaults to the fast-test CPU multiplier
// (`?test=1&fast=1` -- see app.js's TEST_CPU_MULTIPLIER) so the suite isn't
// paying a real POST + FreeDOS boot at genuine 66 MHz on every test. This
// is the one test that deliberately opts back out (`realtime: true`), to
// confirm the underlying "genuine, wall-clock-paced 66 MHz" contract
// (CLAUDE.md's "Never speed these up") actually holds. See CLAUDE.md
// "Current sanctioned overrides" (automated-test CPU clock multiplier).
test.describe("real-speed smoke test", () => {
  test("the guest CPU runs at real, wall-clock-paced 66 MHz -- not sped up", async ({ page }) => {
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
    // checking for genuine ~66 MHz pacing, not tight timing precision, and
    // specifically that it's nowhere near the fast-test multiplier's rate.
    expect(cyclesPerSecond).toBeGreaterThan(33_000_000);
    expect(cyclesPerSecond).toBeLessThan(99_000_000);
  });

  // The throughput assertion above passed throughout the entire period the
  // browser build was unusable (PC486_REVIEW.md §8): at 59 M cycles/sec the
  // machine sat inside that band while being 10% too slow to hold real
  // time, and a cycles-per-second average says nothing about how that work
  // is distributed. Real-time emulation has two requirements and the band
  // only covers the first -- enough throughput, AND no single synchronous
  // call long enough to starve input. This is the second one.
  //
  // runCycles() is synchronous: for its whole duration the main thread
  // dispatches no keydown, no click, and no rAF callback. Before §8.4 split
  // the run loop into wall-clock-bounded chunks, a real boot spent ~23 of
  // every 25 seconds inside ~300ms uninterruptible calls arriving back to
  // back -- which is what made keystrokes take seconds to land or vanish.
  test("no single task blocks the main thread long enough to starve input", async ({ page }) => {
    // Real speed, not the fast multiplier: this is about the experience an
    // actual visitor gets. Installed before navigation so it observes the
    // whole page lifetime.
    await page.addInitScript(() => {
      (window as any).__longTasks = [];
      new PerformanceObserver((list) => {
        for (const e of list.getEntries())
          (window as any).__longTasks.push({ at: e.startTime, dur: e.duration });
      }).observe({ entryTypes: ["longtask"] });
    });
    await boot(page, { realtime: true, expectScreen: null });
    // Let the machine run a real stretch of its boot under observation.
    await page.waitForTimeout(15_000);

    const tasks: { at: number; dur: number }[] = await page.evaluate(
      () => (window as any).__longTasks
    );
    // Page load itself (wasm instantiation plus mounting 947MB of disc
    // images) is genuinely one long task and is not what this guards --
    // see PC486_REVIEW.md §8.5. Everything after the machine is up is.
    const running = tasks.filter((t) => t.at > 3_000);
    const worst = running.reduce((a, t) => (t.dur > a ? t.dur : a), 0);
    expect(
      worst,
      `longest blocking task while running was ${worst.toFixed(0)}ms ` +
        `(all: ${running.map((t) => `${(t.at / 1000).toFixed(1)}s=${t.dur.toFixed(0)}ms`).join(" ")})`
    ).toBeLessThan(150);
  });
});
