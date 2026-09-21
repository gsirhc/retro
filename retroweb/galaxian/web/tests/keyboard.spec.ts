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

  test("released state is the factory idle on all three ports", async ({ page }) => {
    expect(await in0(page)).toBe(0x00);
    expect(await in1(page)).toBe(0x00);
    expect(await page.evaluate(() => (window as any).__test.machine.in2())).toBe(0x04);
  });

  test("ArrowRight / ArrowLeft set IN0 bits 0x08 / 0x04", async ({ page }) => {
    await page.keyboard.down("ArrowRight");
    expect(await in0(page)).toBe(0x08);
    await page.keyboard.up("ArrowRight");
    await page.keyboard.down("ArrowLeft");
    expect(await in0(page)).toBe(0x04);
    await page.keyboard.up("ArrowLeft");
    expect(await in0(page)).toBe(0x00);
  });

  test("AD maps to the same joystick bits", async ({ page }) => {
    await page.keyboard.down("KeyA");
    expect(await in0(page)).toBe(0x04);
    await page.keyboard.up("KeyA");
    await page.keyboard.down("KeyD");
    expect(await in0(page)).toBe(0x08);
    await page.keyboard.up("KeyD");
  });

  test("5 inserts a coin (IN0 bit 0x01)", async ({ page }) => {
    await page.keyboard.down("Digit5");
    expect(await in0(page)).toBe(0x01);
    await page.keyboard.up("Digit5");
    expect(await in0(page)).toBe(0x00);
  });

  test("1 and 2 start 1P/2P (IN1 bits 0x01/0x02)", async ({ page }) => {
    await page.keyboard.down("Digit1");
    expect(await in1(page)).toBe(0x01);
    await page.keyboard.up("Digit1");
    expect(await in1(page)).toBe(0x00);

    await page.keyboard.down("Digit2");
    expect(await in1(page)).toBe(0x02);
    await page.keyboard.up("Digit2");
    expect(await in1(page)).toBe(0x00);
  });

  test("numpad 5/1/2 map to the same coin and start bits", async ({ page }) => {
    await page.keyboard.down("Numpad5");
    expect(await in0(page)).toBe(0x01);
    await page.keyboard.up("Numpad5");
    await page.keyboard.down("Numpad1");
    expect(await in1(page)).toBe(0x01);
    await page.keyboard.up("Numpad1");
    await page.keyboard.down("Numpad2");
    expect(await in1(page)).toBe(0x02);
    await page.keyboard.up("Numpad2");
  });

  test("Z / Space fire sets IN0 bit 0x10", async ({ page }) => {
    await page.keyboard.down("KeyZ");
    expect(await in0(page)).toBe(0x10);
    await page.keyboard.up("KeyZ");
    await page.keyboard.down("Space");
    expect(await in0(page)).toBe(0x10);
    await page.keyboard.up("Space");
    expect(await in0(page)).toBe(0x00);
  });
});
