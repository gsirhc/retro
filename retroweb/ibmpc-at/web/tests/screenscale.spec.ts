import { test, expect } from "./fixtures";
import type { Page } from "@playwright/test";
import { boot } from "./helpers";

// The guest frame is scaled by a whole factor per axis (nearest) into #screen, then smooth-scaled
// to the CSS box. The box is 4:3 in every mode, like the 5154 Enhanced Color Display.

async function chooseTheme(page: Page, theme: string, mode = "light") {
  await page.locator("#pageThemeBtn").click();
  await page.locator(`#themeDialog input[name="pageThemeFamily"][value="${theme}"]`).check();
  await page.locator(`#themeDialog input[name="pageThemeMode"][value="${mode}"]`).check();
  await page.locator("#themeDialogDone").click();
}

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

const axisScale = (box: number, dpr: number, n: number) => Math.min(4, Math.max(1, Math.ceil(box * dpr / n - 0.01)));

function expectSharpBilinear(m: Awaited<ReturnType<typeof measure>>) {
  const kx = m.cw / m.fw, ky = m.ch / m.fh;
  expect(Number.isInteger(kx)).toBe(true);
  expect(Number.isInteger(ky)).toBe(true);
  expect(kx).toBe(axisScale(m.cssW, m.dpr, m.fw));
  expect(ky).toBe(axisScale(m.cssH, m.dpr, m.fh));
  expect(m.rendering).toBe("auto");
}

test.describe("screen scaling", () => {
  test("the prompt fills a 4:3 box at a whole-factor backing scale", async ({ page }) => {
    await boot(page);
    const m = await measure(page);
    expect(m.cssW).toBeGreaterThan(m.fw);
    expect(m.cssW).toBeLessThanOrEqual(860);
    expect(m.cssW / m.cssH).toBeCloseTo(4 / 3, 2);
    expectSharpBilinear(m);
  });

  test("modern theme on a wide window fills its column past 860px and re-fits", async ({ page }) => {
    await boot(page);
    const vp = page.viewportSize()!;
    await page.setViewportSize({ width: 1920, height: 1080 });
    await chooseTheme(page, "modern");
    await expect.poll(async () => (await measure(page)).cssW).toBeGreaterThan(860);
    await expect.poll(async () => { const m = await measure(page); return m.cw / m.fw === axisScale(m.cssW, m.dpr, m.fw); }).toBe(true);
    const m = await measure(page);
    expect(m.cssW / m.cssH).toBeCloseTo(4 / 3, 2);
    expectSharpBilinear(m);
    await chooseTheme(page, "win");
    await page.setViewportSize(vp);
  });

  test("nothing is drawn over the screen: no scanline or vignette layer", async ({ page }) => {
    await boot(page);
    await expect(page.locator("#bezel .crt-overlay")).toHaveCount(0);
    const covered = await page.evaluate(() => {
      const el = (window as any).__test.screenEl as HTMLCanvasElement;
      const r = el.getBoundingClientRect();
      return document.elementFromPoint(r.left + r.width / 2, r.top + r.height / 2) !== el;
    });
    expect(covered).toBe(false);
  });

  test("each guest pixel becomes an exact kx-by-ky block in the backing canvas", async ({ page }) => {
    await boot(page);
    const ok = await page.evaluate(() => {
      const t = (window as any).__test;
      const fc = t.frameCanvas as HTMLCanvasElement;
      const el = t.screenEl as HTMLCanvasElement;
      const kx = el.width / fc.width, ky = el.height / fc.height;
      const src = fc.getContext("2d")!.getImageData(0, 0, fc.width, fc.height).data;
      const dst = el.getContext("2d")!.getImageData(0, 0, el.width, el.height).data;
      for (let y = 0; y < fc.height; y += 7) {
        for (let x = 0; x < fc.width; x += 3) {
          const s = (y * fc.width + x) * 4;
          for (let dy = 0; dy < ky; dy++) for (let dx = 0; dx < kx; dx++) {
            const d = ((y * ky + dy) * el.width + x * kx + dx) * 4;
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
      await boot(page);
      await expect.poll(async () => { const m = await measure(page); return m.cw / m.fw === axisScale(m.cssW, 2, m.fw); }).toBe(true);
      const m = await measure(page);
      expect(m.dpr).toBe(2);
      expect(m.cw / m.fw).toBeGreaterThan(2);
      expectSharpBilinear(m);
    } finally {
      await context.close();
    }
  });
});
