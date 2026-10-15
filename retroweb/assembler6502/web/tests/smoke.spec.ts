import { test, expect } from "@playwright/test";

// Boots the unmodified firmware to Wozmon (twin of machine_test.cpp's
// BootsTheRealFirmwareStraightToWozmon)

test("boots the real ROM straight to the Wozmon prompt", async ({ page }) => {
  const errors: string[] = [];
  page.on("pageerror", (e) => errors.push(e.message));

  await page.goto("/");
  await expect(page.locator("#screen .xterm-rows")).toContainText("\\", { timeout: 45000 });
  // RESET just clears the LCD
  expect(await page.evaluate(() => (window as any).__machine.lcdText())).toBe(" ".repeat(32));

  expect(errors).toEqual([]);
});

test("a real examine command round-trips through the terminal", async ({ page }) => {
  await page.goto("/");
  await expect(page.locator("#screen .xterm-rows")).toContainText("\\", { timeout: 45000 });
  await page.click("#screen");
  // One-byte ACIA RX register: type with a per-char delay so the NMI handler can drain
  await page.keyboard.type("0.F", { delay: 100 });
  await page.keyboard.press("Enter");
  await expect(page.locator("#screen .xterm-rows")).toContainText("0000:", { timeout: 20000 });
});

test("the CPU runs at real, wall-clock-paced 1 MHz -- not sped up", async ({ page }) => {
  await page.goto("/");
  await expect(page.locator("#screen .xterm-rows")).toContainText("\\", { timeout: 45000 });

  const sample = () => page.evaluate(() => [(window as any).__machine.cycleCount(), performance.now()]);
  const [c0, t0] = await sample();
  await page.waitForFunction((t) => performance.now() >= t, t0 + 2000);
  const [c1, t1] = await sample();

  const cyclesPerSecond = (c1 - c0) / ((t1 - t0) / 1000);
  // Slow CI runners only lower the rate; the frame clamp keeps a correct build under 1.1x.
  expect(cyclesPerSecond).toBeGreaterThan(500_000);
  expect(cyclesPerSecond).toBeLessThan(1_100_000);
});
