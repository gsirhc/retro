import { test, expect } from "@playwright/test";

// After the generated self-test ROM's crosshatch + color-bar patterns
// (~1.5 s each), it holds a help screen in its own 8×8 font explaining
// that Namco Pac-Man is not included. Never the copyrighted ROM.

test("self-test ROM ends on a copyright / load-it-yourself help screen", async ({ page }) => {
  await page.goto("/?test=1");
  await expect(page.locator("#romStatus")).toHaveText("Running test ROM");

  // 90+90 frames at ~60.6 Hz is ~3 s, plus a little for the copies.
  await page.waitForTimeout(4500);

  const stats = await page.evaluate(() => {
    const c = document.getElementById("screen") as HTMLCanvasElement;
    const data = c.getContext("2d")!.getImageData(0, 0, c.width, c.height).data;
    let yellow = 0, white = 0, lit = 0;
    for (let i = 0; i < data.length; i += 4) {
      const r = data[i], g = data[i + 1], b = data[i + 2];
      if (r | g | b) lit++;
      if (r > 180 && g > 180 && b < 40) yellow++;
      if (r > 180 && g > 180 && b > 180) white++;
    }
    return { yellow, white, lit };
  });
  // Title is yellow, body is white — both must actually paint.
  expect(stats.yellow).toBeGreaterThan(80);
  expect(stats.white).toBeGreaterThan(200);
  // Sparse text, not a full-screen color-bar fill.
  expect(stats.lit).toBeLessThan(224 * 288 * 0.35);
});
