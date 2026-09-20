import { test, expect } from "@playwright/test";

test.describe("keyboard input", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await page.locator("#screen").click();
  });

  const in0 = (page: import("@playwright/test").Page) =>
    page.evaluate(() => (window as any).__test.machine.in0());
  const in1 = (page: import("@playwright/test").Page) =>
    page.evaluate(() => (window as any).__test.machine.in1());
  const in2 = (page: import("@playwright/test").Page) =>
    page.evaluate(() => (window as any).__test.machine.in2());

  test("released state is the factory idle on all three ports", async ({ page }) => {
    expect(await in0(page)).toBe(0xFF);
    expect(await in1(page)).toBe(0xFC);
    expect(await in2(page)).toBe(0xF1);
  });

  test("ArrowRight / ArrowLeft clear IN0 bits 0x10 / 0x20", async ({ page }) => {
    await page.keyboard.down("ArrowRight");
    expect(await in0(page)).toBe(0xFF & ~0x10);
    await page.keyboard.up("ArrowRight");
    await page.keyboard.down("ArrowLeft");
    expect(await in0(page)).toBe(0xFF & ~0x20);
    await page.keyboard.up("ArrowLeft");
    expect(await in0(page)).toBe(0xFF);
  });

  test("ArrowUp / ArrowDown clear IN2 bits 0x10 / 0x40", async ({ page }) => {
    await page.keyboard.down("ArrowUp");
    expect(await in2(page)).toBe(0xF1 & ~0x10);
    await page.keyboard.up("ArrowUp");
    await page.keyboard.down("ArrowDown");
    expect(await in2(page)).toBe(0xF1 & ~0x40);
    await page.keyboard.up("ArrowDown");
    expect(await in2(page)).toBe(0xF1);
  });

  test("WASD maps to the same joystick bits", async ({ page }) => {
    await page.keyboard.down("KeyW");
    expect(await in2(page)).toBe(0xF1 & ~0x10);
    await page.keyboard.up("KeyW");
    await page.keyboard.down("KeyA");
    expect(await in0(page)).toBe(0xFF & ~0x20);
    await page.keyboard.up("KeyA");
  });

  test("5 inserts a coin (IN0 bit 0x80)", async ({ page }) => {
    await page.keyboard.down("Digit5");
    expect(await in0(page)).toBe(0xFF & ~0x80);
    await page.keyboard.up("Digit5");
    expect(await in0(page)).toBe(0xFF);
  });

  test("1 and 2 start 1P/2P (IN1 bits 0x80/0x40)", async ({ page }) => {
    await page.keyboard.down("Digit1");
    expect(await in1(page)).toBe(0xFC & ~0x80);
    await page.keyboard.up("Digit1");
    expect(await in1(page)).toBe(0xFC);

    await page.keyboard.down("Digit2");
    expect(await in1(page)).toBe(0xFC & ~0x40);
    await page.keyboard.up("Digit2");
    expect(await in1(page)).toBe(0xFC);
  });

  test("numpad 5/1/2 map to the same coin and start bits", async ({ page }) => {
    await page.keyboard.down("Numpad5");
    expect(await in0(page)).toBe(0xFF & ~0x80);
    await page.keyboard.up("Numpad5");
    await page.keyboard.down("Numpad1");
    expect(await in1(page)).toBe(0xFC & ~0x80);
    await page.keyboard.up("Numpad1");
    await page.keyboard.down("Numpad2");
    expect(await in1(page)).toBe(0xFC & ~0x40);
    await page.keyboard.up("Numpad2");
  });
});
