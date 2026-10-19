import { test, expect, type Page } from "@playwright/test";

// After the diagnostic (~1.6 s) the self-test ROM points the AVG at a help list.

const ram = (page: Page, addr: number) => page.evaluate((a) => (window as any).__test.machine.ramByte(a), addr);

test("self-test ROM runs the diagnostic, then switches vector RAM to the help screen", async ({ page }) => {
  await page.goto("/?test=1");
  await expect(page.locator("#romStatus")).toHaveText("Running test ROM");
  await page.waitForFunction(() => !!(window as any).__test?.machine);

  await page.waitForFunction(() => {
    const m = (window as any).__test.machine;
    return m.frames() < 100 && m.ramByte(0x2001) >> 5 === 7;
  });

  await page.waitForFunction(() => (window as any).__test.machine.frames() > 420, null, { timeout: 8000 });
  expect((await ram(page, 0x2001)) >> 5).toBe(5);
  expect((await ram(page, 0x2003)) >> 5).toBe(5);
  expect(await ram(page, 0x2004)).toBe(0x00);
  expect(await ram(page, 0x2005)).toBe(0x20);

  const n = await page.evaluate(async () => {
    const m = (window as any).__test.machine;
    const a = m.avgFrames();
    while (m.avgFrames() < a + 3) await new Promise((r) => requestAnimationFrame(r));
    return m.vectorSegments().length / 5;
  });
  expect(n).toBeGreaterThan(300);
});

test("the Math Box self-check passes", async ({ page }) => {
  await page.goto("/?test=1");
  await page.waitForFunction(() => !!(window as any).__test?.machine);
  await page.waitForFunction(() => (window as any).__test.machine.frames() > 60);
  await expect.poll(() => ram(page, 0x10)).toBe(1);
  expect(await ram(page, 0x11)).toBeGreaterThan(0);
  expect(await page.evaluate(() => (window as any).__test.machine.mathboxSteps())).toBeGreaterThan(0);
});

test("the canvas is not a full-screen fill", async ({ page }) => {
  await page.goto("/?test=1");
  await page.waitForFunction(() => (window as any).__test?.machine?.frames() > 420);
  const stats = await page.evaluate(() => {
    const c = document.getElementById("screen") as HTMLCanvasElement;
    const data = c.getContext("2d")!.getImageData(0, 0, c.width, c.height).data;
    let lit = 0;
    for (let i = 0; i < data.length; i += 4) {
      if (data[i] || data[i + 1] || data[i + 2]) lit++;
    }
    return { lit, total: c.width * c.height };
  });
  expect(stats.lit).toBeGreaterThan(400);
  expect(stats.lit).toBeLessThan(stats.total * 0.25);
});
