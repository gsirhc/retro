import { test, expect } from "./fixtures";
import { bootLive, wallWait } from "./helpers";

// __test is set in the same step that lights the power LED.
test("?fast=1 without ?test=1 exposes no test seam", async ({ page }) => {
  await page.goto("/?fast=1");
  await expect(page.locator("#powerLed")).toHaveClass(/power-on/, { timeout: 15_000 });
  expect(await page.evaluate(() => (window as any).__test)).toBeUndefined();
});

// 20x of 66 MHz is past what a host can run, so the rate alone can't catch a wrong multiplier.
test("?test=1 with fast set to anything but 1 stays at the real 66 MHz", async ({ page }) => {
  await bootLive(page, { realtime: true, params: "fast=2" });
  expect(await page.evaluate(() => (window as any).__test.cpuMultiplier)).toBe(1);
  await wallWait(page, 1000);
  const sample = () => page.evaluate(() => [(window as any).__test.machine.totalCycles(), performance.now()]);
  const [c0, t0] = await sample();
  await wallWait(page, 2000);
  const [c1, t1] = await sample();
  expect((c1 - c0) / ((t1 - t0) / 1000)).toBeLessThan(72_600_000);
});

test("?test=1&fast=1 is the 20x path", async ({ page }) => {
  await bootLive(page);
  expect(await page.evaluate(() => (window as any).__test.cpuMultiplier)).toBe(20);
});
