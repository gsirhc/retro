import { test, expect } from "@playwright/test";

test("self-test ROM ends on a copyright / load-it-yourself help screen", async ({ page }) => {
  await page.goto("/?test=1");
  await expect(page.locator("#romStatus")).toHaveText("Running test ROM");
  await page.waitForTimeout(4500);

  const stats = await page.evaluate(() => {
    const c = document.getElementById("screen") as HTMLCanvasElement;
    const data = c.getContext("2d")!.getImageData(0, 0, c.width, c.height).data;
    let yellow = 0, white = 0, lit = 0;
    for (let i = 0; i < data.length; i += 4) {
      const r = data[i], g = data[i + 1], b = data[i + 2];
      if (r | g | b) lit++;
      if (r > 180 && g > 180 && b < 40) yellow++;
      if (r > 180 && g > 180 && b > 80) white++;
    }
    return { yellow, white, lit };
  });
  expect(stats.yellow).toBeGreaterThan(40);
  expect(stats.white).toBeGreaterThan(80);
  expect(stats.lit).toBeLessThan(224 * 256 * 0.35);
});
