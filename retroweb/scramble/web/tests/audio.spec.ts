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

  test("an AY tone reaches the speaker at the Konami netlist level", async ({ page }) => {
    await page.evaluate(() => { (window as any).__audioPeak = 0; });
    await page.waitForTimeout(300);
    const idle = await page.evaluate(() => (window as any).__audioPeak);
    await page.evaluate(() => {
      const m = (window as any).__test.machine;
      const reg = (r: number, v: number) => { m.soundOut(0x40, r); m.soundOut(0x80, v); };
      reg(0, 0x80);
      reg(1, 0x00);
      reg(7, 0x3e);
      reg(8, 0x0f);
      (window as any).__audioPeak = 0;
    });
    await page.waitForTimeout(500);
    const peak = await page.evaluate(() => (window as any).__audioPeak);
    expect(idle).toBeLessThan(1e-3);
    expect(peak).toBeGreaterThan(0.65);
    expect(peak).toBeLessThan(1.0);
  });
});
