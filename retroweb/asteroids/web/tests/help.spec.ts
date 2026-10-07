import { test, expect } from "@playwright/test";

// Self-test picture: sparse line art (roms/hwtest/gen_hwtest.py).

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
  // Canvas may be DPR-backed, so compare against backing pixels.
  expect(stats.lit).toBeGreaterThan(400);
  expect(stats.lit).toBeLessThan(stats.total * 0.25);
});
