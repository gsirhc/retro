import { test, expect } from "./fixtures";
import { bootLive } from "./helpers";

// Real-speed pacing check. Every other spec boots under `?test=1&fast=1` (app.js
// TEST_CPU_MULTIPLIER); this one passes `realtime: true` to confirm the wall-clock-paced 66 MHz holds.
test.describe("real-speed smoke test", () => {
  test("the guest CPU runs at real, wall-clock-paced 66 MHz -- not sped up", async ({ page }) => {
    // Skip the slow live-prompt wait; the machine only needs to be running.
    await bootLive(page, { realtime: true });
    // Let start-up settle first (wasm tier-up, the first C: write to IndexedDB).
    await page.waitForTimeout(1000);

    const c0 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    const t0 = Date.now();
    await page.waitForTimeout(2000);
    const c1 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    const t1 = Date.now();

    const cyclesPerSecond = (c1 - c0) / ((t1 - t0) / 1000);
    // Generous tolerance for CI jitter; the check is ~66 MHz pacing, nowhere near the fast multiplier.
    expect(cyclesPerSecond).toBeGreaterThan(33_000_000);
    expect(cyclesPerSecond).toBeLessThan(99_000_000);
  });

  // The throughput band above passed while the browser build was unusable (PC486_REVIEW.md §8): 59 M
  // cycles/sec was 10% too slow for real time. Real time also needs no synchronous call long enough to
  // starve input. runCycles() blocks keydown/click/rAF; before §8.4 chunked it, a boot spent ~23 of
  // every 25 seconds in ~300ms calls.
  test("no single task blocks the main thread long enough to starve input", async ({ page }) => {
    // Real speed, installed before navigation to observe the whole page lifetime.
    await page.addInitScript(() => {
      (window as any).__longTasks = [];
      new PerformanceObserver((list) => {
        for (const e of list.getEntries())
          (window as any).__longTasks.push({ at: e.startTime, dur: e.duration });
      }).observe({ entryTypes: ["longtask"] });
    });
    await bootLive(page, { realtime: true });
    await page.waitForTimeout(15_000);

    const tasks: { at: number; dur: number }[] = await page.evaluate(
      () => (window as any).__longTasks
    );
    // Page load (wasm instantiation, mounting 947MB of discs) is one long task and not guarded; see PC486_REVIEW.md §8.5.
    const running = tasks.filter((t) => t.at > 3_000);
    const worst = running.reduce((a, t) => (t.dur > a ? t.dur : a), 0);
    expect(
      worst,
      `longest blocking task while running was ${worst.toFixed(0)}ms ` +
        `(all: ${running.map((t) => `${(t.at / 1000).toFixed(1)}s=${t.dur.toFixed(0)}ms`).join(" ")})`
    ).toBeLessThan(150);
  });
});
