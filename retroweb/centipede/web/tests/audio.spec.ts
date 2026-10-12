import { test, expect } from "@playwright/test";

test.describe("Audio latency", () => {
  test("main-thread stalls do not leave the sound queued behind the picture", async ({ page }) => {
    await page.addInitScript(() => {
      const w = window as any;
      w.__queued = [];
      const RealBlob = window.Blob;
      (window as any).Blob = function (parts: any[], opts: any) {
        if (typeof parts[0] === "string" && parts[0].includes("registerProcessor")) {
          const report = "if ((this.n = (this.n || 0) + 1) % 40 === 0) { let s = -this.i; for (const c of this.q) s += c.length; this.port.postMessage(s); } return true;";
          parts = [parts[0].replace("return true;", report)];
        }
        return new RealBlob(parts, opts);
      } as any;
      const RealNode = window.AudioWorkletNode;
      (window as any).AudioWorkletNode = function (ctx: any, name: string) {
        const n = new RealNode(ctx, name);
        w.__rate = ctx.sampleRate;
        n.port.onmessage = (e: MessageEvent) => w.__queued.push(e.data);
        return n;
      } as any;
    });
    await page.goto("/?test=1");
    await page.waitForFunction(() => (window as any).__test);
    await page.locator("body").click({ position: { x: 5, y: 5 } });
    await page.waitForFunction(() => (window as any).__queued.length > 5);
    for (let k = 0; k < 8; k++) {
      await page.evaluate(() => { const t = performance.now(); while (performance.now() - t < 60) {} });
      await page.waitForTimeout(200);
    }
    await page.evaluate(() => { (window as any).__queued = []; });
    await page.waitForTimeout(1000);
    const maxMs = await page.evaluate(() => { const w = window as any; return Math.max(...w.__queued) / w.__rate * 1000; });
    expect(maxMs).toBeLessThan(60);
  });
});
