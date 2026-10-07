import { test, expect } from "@playwright/test";

// Boots the built-in hardware self-test ROM (never Atari Asteroids) and
// checks the real, wall-clock-paced 1.512 MHz 6502 core is actually running:
// the RAM signature lands, the DVG paints a non-black frame, and
// cycles/real-second lands near the genuine clock rate -- see CLAUDE.md's
// "Never speed these up". Native board coverage is this machine's own
// tests/smoke_test.cpp; this file only smokes the board in a browser.

test("boots the test ROM, writes the AST1 RAM signature, paints a non-black frame", async ({ page }) => {
  const errors: string[] = [];
  page.on("pageerror", (e) => errors.push(e.message));

  await page.goto("/?test=1");
  await expect(page.locator("#romStatus")).toHaveText("Running test ROM");

  // The self-test ROM writes its signature in its first few opcodes after
  // reset -- give it a couple of real frames, then check RAM directly.
  await page.waitForFunction(() => {
    const m = (window as any).__test?.machine;
    if (!m) return false;
    return m.ramByte(0) === 0x41 && m.ramByte(1) === 0x53 &&
           m.ramByte(2) === 0x54 && m.ramByte(3) === 0x31; // 'A' 'S' 'T' '1'
  }, null, { timeout: 5000 });

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

test("the monitor is 4:3 and fills about 75% of the viewport height when not fullscreen", async ({ page }) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.goto("/");
  const box = await page.locator("#screen").boundingBox();
  expect(box).toBeTruthy();
  // 75% of 800px = 600; allow bezel/page chrome and the max-width clamp
  // to shave a little.
  expect(box!.height).toBeGreaterThan(500);
  expect(box!.height).toBeLessThan(650);
  expect(Math.abs(box!.width / box!.height - 4 / 3)).toBeLessThan(0.02);
});

test("the guest CPU runs at real, wall-clock-paced 1.512 MHz -- not sped up", async ({ page }) => {
  await page.goto("/?test=1");
  await expect(page.locator("#romStatus")).toHaveText("Running test ROM");

  const c0 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
  const t0 = Date.now();
  await page.waitForTimeout(2000);
  const c1 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
  const t1 = Date.now();

  const cyclesPerSecond = (c1 - c0) / ((t1 - t0) / 1000);
  // Generous tolerance for CI scheduling jitter -- checking for genuine
  // ~1.512 MHz pacing, not tight timing precision.
  expect(cyclesPerSecond).toBeGreaterThan(750_000);
  expect(cyclesPerSecond).toBeLessThan(2_300_000);
});
