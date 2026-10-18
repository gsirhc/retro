import { test, expect } from "@playwright/test";

const counts = () => {
  const c = document.getElementById("screen") as HTMLCanvasElement;
  const data = c.getContext("2d")!.getImageData(0, 0, c.width, c.height).data;
  let red = 0, green = 0, yellow = 0, white = 0, lit = 0;
  for (let i = 0; i < data.length; i += 4) {
    const r = data[i], g = data[i + 1], b = data[i + 2];
    if (r | g | b) lit++;
    if (r === 255 && !g && !b) red++;
    if (g === 255 && !r && !b) green++;
    if (r === 255 && g === 255 && !b) yellow++;
    if (r === 255 && g === 255 && b === 255) white++;
  }
  return { red, green, yellow, white, lit };
};

const pixels = (points: [number, number][]) => {
  const c = document.getElementById("screen") as HTMLCanvasElement;
  const g = c.getContext("2d")!;
  return points.map(([x, y]) => Array.from(g.getImageData(x, y, 1, 1).data.slice(0, 3)));
};

test("self-test ROM ends on a copyright / load-it-yourself help screen", async ({ page }) => {
  await page.goto("/?test=1");
  await expect(page.locator("#romStatus")).toHaveText("Running test ROM");
  await page.waitForFunction(() => !!(window as any).__test?.machine);
  await page.waitForFunction(() => (window as any).__test?.machine.state().frames >= 240);
  await page.evaluate(() => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))));

  const stats = await page.evaluate(counts);
  expect(stats.white).toBeGreaterThan(200);
  expect(stats.green).toBeGreaterThan(80);
  expect(stats.yellow).toBeGreaterThan(80);
  expect(stats.red).toBeGreaterThan(40);
  expect(stats.lit).toBeLessThan(224 * 260 * 0.2);
});

test("the colour PROM paints the full-white pattern as horizontal bands", async ({ page }) => {
  await page.goto("/?test=1");
  await page.waitForFunction(() => !!(window as any).__test?.machine);
  const points: [number, number][] = [[100, 4], [100, 40], [100, 72], [100, 104], [100, 200], [100, 258]];
  const handle = await page.waitForFunction((pts) => {
    const c = document.getElementById("screen") as HTMLCanvasElement;
    const g = c.getContext("2d")!;
    const px = pts.map(([x, y]) => Array.from(g.getImageData(x, y, 1, 1).data.slice(0, 3)));
    return px[1][1] === 255 && !px[1][0] && !px[1][2] ? px : null;
  }, points);
  const px = (await handle.jsonValue())!;
  expect(px[0]).toEqual([255, 255, 255]);
  expect(px[1]).toEqual([0, 255, 0]);
  expect(px[2]).toEqual([255, 255, 0]);
  expect(px[3]).toEqual([0, 255, 255]);
  expect(px[4]).toEqual([255, 0, 0]);
  expect(px[5]).toEqual([0, 0, 0]);
});

test("OUT 3 bit 2 turns the whole screen red", async ({ page }) => {
  await page.goto("/?test=1");
  await page.waitForFunction(() => !!(window as any).__test?.machine);
  await page.waitForFunction(() => (window as any).__test.machine.state().red);
  const points: [number, number][] = [[100, 4], [100, 40], [100, 72], [100, 200]];
  await page.waitForFunction((pts) => {
    const c = document.getElementById("screen") as HTMLCanvasElement;
    const g = c.getContext("2d")!;
    return pts.every(([x, y]) => {
      const d = g.getImageData(x, y, 1, 1).data;
      return d[0] === 255 && !d[1] && !d[2];
    });
  }, points);
  expect(await page.evaluate(pixels, points)).toEqual(points.map(() => [255, 0, 0]));
});
