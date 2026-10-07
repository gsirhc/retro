import { test, expect } from "@playwright/test";

// Hardware self-test picture: border, crosshair, nested boxes, intensity
// ramp, plus JSR'd diamond/ship from vector ROM (roms/hwtest/gen_hwtest.py).
// Sparse line art — not a full-screen fill and not a blank tube.

test("self-test ROM draws a sparse vector diagnostic, not a full-screen fill", async ({ page }) => {
  await page.goto("/?test=1");
  await expect(page.locator("#romStatus")).toHaveText("Running test ROM");

  await page.waitForFunction(() => {
    const m = (window as any).__test?.machine;
    return !!m && m.frames() > 2;
  }, null, { timeout: 5000 });

  const stats = await page.evaluate(() => {
    const c = document.getElementById("screen") as HTMLCanvasElement;
    const data = c.getContext("2d")!.getImageData(0, 0, c.width, c.height).data;
    let lit = 0;
    for (let i = 0; i < data.length; i += 4) {
      if (data[i] || data[i + 1] || data[i + 2]) lit++;
    }
    return { lit, total: c.width * c.height };
  });
  // Richer than the old single box, still sparse vector art. Canvas may
  // be DPR-backed (width > CSS size), so compare against backing pixels.
  expect(stats.lit).toBeGreaterThan(400);
  expect(stats.lit).toBeLessThan(stats.total * 0.25);
});
