import { test, expect } from "./fixtures";
import type { Page } from "@playwright/test";
import { bootLive } from "./helpers";

// The guest frame is blown up by a whole factor (nearest-neighbour) into
// #screen, then smooth-scaled to the CSS box. A plain pixelated stretch to
// 860px doubles every third column and garbles text on a 1x display.

async function measure(page: Page) {
  return page.evaluate(() => {
    const t = (window as any).__test;
    const el = t.screenEl as HTMLCanvasElement;
    const fc = t.frameCanvas as HTMLCanvasElement;
    const r = el.getBoundingClientRect();
    return {
      dpr: window.devicePixelRatio,
      fw: fc.width, fh: fc.height, cw: el.width, ch: el.height,
      cssW: r.width, cssH: r.height,
      rendering: getComputedStyle(el).imageRendering,
    };
  });
}

function expectSharpBilinear(m: Awaited<ReturnType<typeof measure>>) {
  const k = m.cw / m.fw;
  expect(Number.isInteger(k)).toBe(true);
  expect(m.ch).toBe(m.fh * k);
  expect(k).toBe(Math.min(4, Math.max(1, Math.ceil(m.cssW * m.dpr / m.fw - 0.01))));
  expect(m.rendering).toBe("auto");
}

test.describe("screen scaling", () => {
  test("default theme fills its box at a whole-factor backing scale", async ({ livePage: page }) => {
    const m = await measure(page);
    expect(m.cssW).toBeGreaterThan(m.fw);
    expect(m.cssW).toBeLessThanOrEqual(860);
    expectSharpBilinear(m);
    expect(m.cssW / m.cssH).toBeCloseTo(m.fw / m.fh, 1);
  });

  test("modern theme on a wide window fills its column past 860px and re-fits", async ({ livePage: page }) => {
    const vp = page.viewportSize()!;
    await page.setViewportSize({ width: 1920, height: 1080 });
    await page.locator("#pageTheme").selectOption("modern");
    await expect.poll(async () => (await measure(page)).cssW).toBeGreaterThan(860);
    await expect.poll(async () => { const m = await measure(page); return m.cw / m.fw === Math.ceil(m.cssW * m.dpr / m.fw - 0.01); }).toBe(true);
    expectSharpBilinear(await measure(page));
    await page.locator("#pageTheme").selectOption("win");
    await page.setViewportSize(vp);
  });

  test("each guest pixel becomes an exact k-by-k block in the backing canvas", async ({ livePage: page }) => {
    const ok = await page.evaluate(() => {
      const t = (window as any).__test;
      const fc = t.frameCanvas as HTMLCanvasElement;
      const el = t.screenEl as HTMLCanvasElement;
      const k = el.width / fc.width;
      const src = fc.getContext("2d")!.getImageData(0, 0, fc.width, fc.height).data;
      const dst = el.getContext("2d")!.getImageData(0, 0, el.width, el.height).data;
      for (let y = 0; y < fc.height; y += 7) {
        for (let x = 0; x < fc.width; x += 3) {
          const s = (y * fc.width + x) * 4;
          for (let dy = 0; dy < k; dy++) for (let dx = 0; dx < k; dx++) {
            const d = ((y * k + dy) * el.width + x * k + dx) * 4;
            if (dst[d] !== src[s] || dst[d + 1] !== src[s + 1] || dst[d + 2] !== src[s + 2]) return false;
          }
        }
      }
      return true;
    });
    expect(ok).toBe(true);
  });

  test("a 2x display gets a higher backing scale for the same box", async ({ browser }) => {
    const context = await browser.newContext({ viewport: { width: 1280, height: 720 }, deviceScaleFactor: 2 });
    const page = await context.newPage();
    try {
      await bootLive(page);
      await expect.poll(async () => { const m = await measure(page); return m.cw / m.fw === Math.ceil(m.cssW * 2 / m.fw - 0.01); }).toBe(true);
      const m = await measure(page);
      expect(m.dpr).toBe(2);
      expect(m.cw / m.fw).toBeGreaterThan(2);
      expectSharpBilinear(m);
    } finally {
      await context.close();
    }
  });
});
