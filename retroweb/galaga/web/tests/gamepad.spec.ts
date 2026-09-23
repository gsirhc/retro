import { test, expect } from "@playwright/test";

test.describe("gamepad", () => {
  test.beforeEach(async ({ page }) => {
    await page.addInitScript(() => {
      const buttons = Array.from({ length: 16 }, () => ({ pressed: false, value: 0 }));
      const pad = {
        axes: [0, 0, 0, 0],
        buttons,
        connected: true,
        index: 0,
        mapping: "standard",
        id: "test pad",
        timestamp: 0,
      };
      (window as any).__pad = pad;
      navigator.getGamepads = () => [pad as unknown as Gamepad];
    });
    await page.goto("/?test=1");
    await page.waitForFunction(() => (window as any).__test);
  });

  test("stick, A, Start, and Select use the same bits as the keys", async ({ page }) => {
    const read = () => page.evaluate(() => ({
      in0: (window as any).__test.machine.in0(),
      in1: (window as any).__test.machine.in1(),
    }));

    await page.evaluate(() => { (window as any).__pad.axes[0] = 1; });
    await expect.poll(async () => (await read()).in0).toBe(0x02);
    await page.evaluate(() => { (window as any).__pad.axes[0] = -1; });
    await expect.poll(async () => (await read()).in0).toBe(0x08);
    await page.evaluate(() => { (window as any).__pad.axes[0] = 0; });
    await expect.poll(async () => (await read()).in0).toBe(0x00);

    await page.evaluate(() => { (window as any).__pad.buttons[0].pressed = true; });
    await expect.poll(async () => (await read()).in1).toBe(0x01);
    await page.evaluate(() => { (window as any).__pad.buttons[0].pressed = false; });

    await page.evaluate(() => { (window as any).__pad.buttons[9].pressed = true; });
    await expect.poll(async () => (await read()).in1).toBe(0x04);
    await page.evaluate(() => { (window as any).__pad.buttons[9].pressed = false; });

    await page.evaluate(() => { (window as any).__pad.buttons[8].pressed = true; });
    await expect.poll(async () => (await read()).in1).toBe(0x10);
    await page.evaluate(() => { (window as any).__pad.buttons[8].pressed = false; });
    await expect.poll(async () => (await read()).in1).toBe(0x00);
  });
});
