import { test, expect } from "@playwright/test";

test.describe("Audio worklet queue", () => {
  test.beforeEach(async ({ page }) => {
    await page.addInitScript(() => {
      const RealBlob = window.Blob;
      (window as any).Blob = function (parts: any[], opts: any) {
        if (typeof parts[0] === "string" && parts[0].includes("registerProcessor")) (window as any).__workletSrc = parts[0];
        return new RealBlob(parts, opts);
      } as any;
    });
    await page.goto("/?test=1");
    await page.waitForFunction(() => (window as any).__test);
    await page.locator("body").click({ position: { x: 5, y: 5 } });
    await page.waitForFunction(() => (window as any).__workletSrc);
  });

  async function feed(page: any, chunks: number[], reads: number) {
    return page.evaluate(({ chunks, reads }: { chunks: number[]; reads: number }) => {
      let Proc: any = null;
      const scope = {
        sampleRate: 48000,
        AudioWorkletProcessor: class { port: any = {}; },
        registerProcessor: (_: string, c: any) => { Proc = c; },
      };
      new Function(...Object.keys(scope), (window as any).__workletSrc)(...Object.values(scope));
      const p = new Proc();
      let next = 0;
      for (const n of chunks) {
        const d = new Float32Array(n);
        for (let k = 0; k < n; k++) d[k] = ++next;
        p.port.onmessage({ data: d });
      }
      const out: number[] = [];
      for (let r = 0; r < reads; r++) {
        const o = new Float32Array(128);
        p.process([], [[o]]);
        out.push(...o);
      }
      return { out, last: next };
    }, { chunks, reads });
  }

  test("half-size chunks from a 120 Hz display all play, in order", async ({ page }) => {
    const { out, last } = await feed(page, [400, 400, 400, 400, 400], 16);
    expect(out.slice(0, last)).toEqual(Array.from({ length: last }, (_, k) => k + 1));
  });

  test("a stalled backlog trims only the oldest samples down to 50 ms", async ({ page }) => {
    const { out, last } = await feed(page, [800, 800, 800, 800], 20);
    const kept = 2400;
    expect(out.slice(0, kept)).toEqual(Array.from({ length: kept }, (_, k) => last - kept + k + 1));
  });
});
