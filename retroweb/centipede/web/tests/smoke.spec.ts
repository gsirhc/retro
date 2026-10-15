import { test, expect } from "@playwright/test";

// Boots the built-in self-test ROM (never Atari Centipede). Checks the board paints
// and cycles per real second land near the genuine 1.512 MHz clock rate.

test("boots the test ROM, writes the CEN1 RAM signature, paints a non-black frame", async ({ page }) => {
  const errors: string[] = [];
  page.on("pageerror", (e) => errors.push(e.message));

  await page.goto("/?test=1");
  await expect(page.locator("#romStatus")).toHaveText("Running test ROM");

  await page.waitForFunction(() => {
    const m = (window as any).__test?.machine;
    if (!m) return false;
    return m.ramByte(0) === 0x43 && m.ramByte(1) === 0x45 &&
           m.ramByte(2) === 0x4e && m.ramByte(3) === 0x31; // 'C' 'E' 'N' '1'
  }, null, { timeout: 5000 });

  await page.waitForTimeout(500);
  const nonBlack = await page.evaluate(() => {
    const c = document.getElementById("screen") as HTMLCanvasElement;
    const data = c.getContext("2d")!.getImageData(0, 0, c.width, c.height).data;
    for (let i = 0; i < data.length; i += 4) {
      if (data[i] || data[i + 1] || data[i + 2]) return true;
    }
    return false;
  });
  expect(nonBlack).toBe(true);

  expect(errors).toEqual([]);
});

test("the monitor is portrait 240:256 and fills about 75% of the viewport height when not fullscreen", async ({ page }) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.goto("/");
  const box = await page.locator("#screen").boundingBox();
  expect(box).toBeTruthy();
  expect(box!.height).toBeGreaterThan(500);
  expect(box!.height).toBeLessThan(650);
  expect(Math.abs(box!.width / box!.height - 240 / 256)).toBeLessThan(0.02);
});

test("the guest CPU runs at real, wall-clock-paced 1.512 MHz -- not sped up", async ({ page }) => {
  await page.goto("/?test=1");
  await expect(page.locator("#romStatus")).toHaveText("Running test ROM");

  const sample = () => page.evaluate(() => [(window as any).__test.machine.totalCycles(), performance.now()]);
  const [c0, t0] = await sample();
  await page.waitForTimeout(2000);
  const [c1, t1] = await sample();

  const cyclesPerSecond = (c1 - c0) / ((t1 - t0) / 1000);
  // Slow CI runners only lower the rate; the frame clamp keeps a correct build under 1.1x.
  expect(cyclesPerSecond).toBeGreaterThan(750_000);
  expect(cyclesPerSecond).toBeLessThan(1_663_000);
});
