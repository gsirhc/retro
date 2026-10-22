import { test, expect } from "@playwright/test";

const counts = () => {
  const c = document.getElementById("screen") as HTMLCanvasElement;
  const data = c.getContext("2d")!.getImageData(0, 0, c.width, c.height).data;
  let red = 0, green = 0, white = 0, lit = 0;
  for (let i = 0; i < data.length; i += 4) {
    const r = data[i], g = data[i + 1], b = data[i + 2];
    if (r | g | b) lit++;
    if (r > 200 && g < 60 && b < 60) red++;
    if (g > 200 && r < 60 && b < 60) green++;
    if (r > 200 && g > 200 && b > 200) white++;
  }
  return { red, green, white, lit };
};

test("self-test ROM ends on a copyright / load-it-yourself help screen", async ({ page }) => {
  await page.goto("/?test=1");
  await expect(page.locator("#romStatus")).toHaveText("Running test ROM");
  await page.waitForFunction(() => !!(window as any).__test?.machine);
  await page.waitForFunction(() => (window as any).__test?.machine.state().frames >= 200);
  await page.evaluate(() => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))));

  const stats = await page.evaluate(counts);
  expect(stats.red).toBeGreaterThan(80);
  expect(stats.green).toBeGreaterThan(80);
  expect(stats.white).toBeGreaterThan(200);
  expect(stats.lit).toBeLessThan(224 * 260 * 0.2);
});

test("the cellophane gels tint the full-white pattern red at the top and green at the bottom", async ({ page }) => {
  await page.goto("/?test=1");
  await page.waitForFunction(() => !!(window as any).__test?.machine);
  const handle = await page.waitForFunction(() => {
    const c = document.getElementById("screen") as HTMLCanvasElement;
    const g = c.getContext("2d")!;
    const at = (x: number, y: number) => Array.from(g.getImageData(x, y, 1, 1).data.slice(0, 3));
    const px = { top: at(100, 10), red: at(100, 45), mid: at(100, 120), green: at(100, 200), base: at(50, 250), corner: at(200, 250), blank: at(100, 258) };
    const white = (p: number[]) => p[0] === 255 && p[1] === 255 && p[2] === 255;
    return white(px.mid) && white(px.top) && white(px.corner) ? px : null;
  });
  const px = (await handle.jsonValue())!;
  expect(px.red).toEqual([255, 0, 0]);
  expect(px.green).toEqual([0, 255, 0]);
  expect(px.base).toEqual([0, 255, 0]);
  expect(px.blank).toEqual([0, 0, 0]);
});
