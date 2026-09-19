import { test, expect } from "@playwright/test";

// Joystick/coin/start input mapping (app.js's applyKeys). IN0/IN1 are
// active-low as the real cabinet edge connector presents them -- released
// reads back 0xFF, a pressed bit reads back 0.

test.describe("keyboard input", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await page.locator("#screen").click();
  });

  const in0 = (page: import("@playwright/test").Page) =>
    page.evaluate(() => (window as any).__test.machine.in0());
  const in1 = (page: import("@playwright/test").Page) =>
    page.evaluate(() => (window as any).__test.machine.in1());

  test("released state reads back 0xFF on both ports", async ({ page }) => {
    expect(await in0(page)).toBe(0xFF);
    expect(await in1(page)).toBe(0xFF);
  });

  const joystickCases: [string, number][] = [
    ["ArrowUp", 0x01],
    ["ArrowLeft", 0x02],
    ["ArrowRight", 0x04],
    ["ArrowDown", 0x08],
  ];
  for (const [key, bit] of joystickCases) {
    test(`${key} clears IN0 bit 0x${bit.toString(16)} while held`, async ({ page }) => {
      await page.keyboard.down(key);
      expect(await in0(page)).toBe(0xFF & ~bit);
      await page.keyboard.up(key);
      expect(await in0(page)).toBe(0xFF);
    });
  }

  test("WASD maps to the same joystick bits as the arrow keys", async ({ page }) => {
    await page.keyboard.down("KeyW");
    expect(await in0(page)).toBe(0xFF & ~0x01);
    await page.keyboard.up("KeyW");
    await page.keyboard.down("KeyA");
    expect(await in0(page)).toBe(0xFF & ~0x02);
    await page.keyboard.up("KeyA");
  });

  test("5 inserts a coin (IN0 bit 0x20)", async ({ page }) => {
    await page.keyboard.down("Digit5");
    expect(await in0(page)).toBe(0xFF & ~0x20);
    await page.keyboard.up("Digit5");
    expect(await in0(page)).toBe(0xFF);
  });

  test("1 and 2 start 1P/2P (IN1 bits 0x20/0x40)", async ({ page }) => {
    await page.keyboard.down("Digit1");
    expect(await in1(page)).toBe(0xFF & ~0x20);
    await page.keyboard.up("Digit1");
    expect(await in1(page)).toBe(0xFF);

    await page.keyboard.down("Digit2");
    expect(await in1(page)).toBe(0xFF & ~0x40);
    await page.keyboard.up("Digit2");
    expect(await in1(page)).toBe(0xFF);
  });
});
