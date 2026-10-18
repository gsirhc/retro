import { test, expect, type Page } from "@playwright/test";

// One audio block is posted per frame, ~60 per second.
const blocks = (page: Page, n: number) => page.evaluate((k) => new Promise<void>((done) => {
  const w = window as any;
  const end = w.__audioBlocks + k;
  const tick = () => (w.__audioBlocks >= end ? done() : requestAnimationFrame(tick));
  tick();
}), n);

async function peakAfter(page: Page, port: number, value: number) {
  await page.evaluate(([p, v]) => {
    (window as any).__test.machine.portOut(p, v);
    (window as any).__audioPeak = 0;
  }, [port, value]);
  await blocks(page, 30);
  return page.evaluate(() => (window as any).__audioPeak);
}

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
    await page.waitForFunction(() => (window as any).__test.machine.state().frames > 200);
    await page.waitForFunction(() => (window as any).__audioBlocks > 10);
  });

  test("the help screen is silent with the amplifier on", async ({ page }) => {
    expect(await page.evaluate(() => (window as any).__test.machine.state().amp)).toBe(true);
    await page.evaluate(() => { (window as any).__audioPeak = 0; });
    await blocks(page, 18);
    expect(await page.evaluate(() => (window as any).__audioPeak)).toBeLessThan(1e-3);
  });

  test("OUT 3 bit 0 runs the SN76477 saucer at its mixer level", async ({ page }) => {
    const peak = await peakAfter(page, 3, 0x21);
    expect(peak).toBeGreaterThan(0.12);
    expect(peak).toBeLessThan(0.26);
  });

  test("OUT 3 bit 5 low mutes the amplifier", async ({ page }) => {
    expect(await peakAfter(page, 3, 0x01)).toBeLessThan(1e-3);
  });

  test("OUT 5 fleet data drives the thump", async ({ page }) => {
    expect(await peakAfter(page, 5, 0x01)).toBeGreaterThan(0.4);
  });
});
