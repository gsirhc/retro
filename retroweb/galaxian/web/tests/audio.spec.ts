import { test, expect } from "@playwright/test";

test.describe("Audio level", () => {
  test.beforeEach(async ({ page }) => {
    await page.addInitScript(() => {
      const w = window as any;
      w.__audioPeak = 0;
      w.__audioBlocks = 0;
      const post = MessagePort.prototype.postMessage;
      MessagePort.prototype.postMessage = function (this: MessagePort, data: any, ...rest: any[]) {
        if (data instanceof Float32Array) {
          w.__audioBlocks++;
          for (const s of data) w.__audioPeak = Math.max(w.__audioPeak, Math.abs(s));
        }
        return (post as any).call(this, data, ...rest);
      };
    });
    await page.goto("/?test=1");
    await page.waitForFunction(() => (window as any).__test);
    await page.locator("body").click({ position: { x: 5, y: 5 } });
    await page.waitForFunction(() => (window as any).__audioBlocks > 10);
  });

  test("an FS voice reaches the speaker at the discrete mixer level", async ({ page }) => {
    await page.evaluate(() => { (window as any).__audioPeak = 0; });
    await page.waitForTimeout(300);
    const idle = await page.evaluate(() => (window as any).__audioPeak);
    await page.evaluate(() => {
      const m = (window as any).__test.machine;
      m.setMemByte(0x6800, 1);
      (window as any).__audioPeak = 0;
    });
    await page.waitForTimeout(500);
    const peak = await page.evaluate(() => (window as any).__audioPeak);
    expect(idle).toBeLessThan(1e-3);
    expect(peak).toBeGreaterThan(0.11);
    expect(peak).toBeLessThan(0.21);
  });
});
