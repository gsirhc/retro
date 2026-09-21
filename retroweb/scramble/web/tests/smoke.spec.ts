import { test, expect } from "@playwright/test";

// Boots the built-in hardware self-test ROM (never Konami Scramble) and
// checks the real, wall-clock-paced 3.072 MHz Z80 core is actually running:
// the tile pattern paints, and cycles/real-second lands near the genuine
// clock rate -- see CLAUDE.md's "Never speed these up". Native ISA coverage
// is retroweb/shared/cpu (zexdoc); this file only smokes the board in a
// browser.

test("boots the test ROM, paints a non-black frame, no console errors", async ({ page }) => {
  const errors: string[] = [];
  page.on("pageerror", (e) => errors.push(e.message));

  await page.goto("/?test=1");
  await expect(page.locator("#romStatus")).toHaveText("Running test ROM");
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

test("the monitor fills about 75% of the viewport height when not fullscreen", async ({ page }) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.goto("/");
  const box = await page.locator("#screen").boundingBox();
  expect(box).toBeTruthy();
  expect(box!.height).toBeGreaterThan(500);
  expect(box!.height).toBeLessThan(650);
  expect(Math.abs(box!.width / box!.height - 224 / 256)).toBeLessThan(0.02);
});

test("the guest CPU runs at real, wall-clock-paced 3.072 MHz -- not sped up", async ({ page }) => {
  await page.goto("/?test=1");
  await expect(page.locator("#romStatus")).toHaveText("Running test ROM");

  const c0 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
  const t0 = Date.now();
  await page.waitForTimeout(2000);
  const c1 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
  const t1 = Date.now();

  const cyclesPerSecond = (c1 - c0) / ((t1 - t0) / 1000);
  expect(cyclesPerSecond).toBeGreaterThan(1_500_000);
  expect(cyclesPerSecond).toBeLessThan(4_500_000);
});
