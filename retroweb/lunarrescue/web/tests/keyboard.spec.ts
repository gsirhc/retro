import { test, expect, type Page } from "@playwright/test";

// Upright PCB: P1 fire/left/right on bits 4/5/6 of IN1 and IN2, IN0 tied high. COIN (IN1 bit 0) is active-low.
const ports = (page: Page) =>
  page.evaluate(() => { const m = (window as any).__test.machine; return [m.in0(), m.in1(), m.in2()]; });

test.describe("keyboard input", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await page.waitForFunction(() => !!(window as any).__test?.machine);
    await page.locator("#screen").click();
  });

  test("released state is the PCB idle on all three ports", async ({ page }) => {
    expect(await ports(page)).toEqual([0xFF, 0x89, 0x00]);
  });

  test("ArrowLeft / ArrowRight set bit 0x20 / 0x40 on IN1 and IN2", async ({ page }) => {
    await page.keyboard.down("ArrowLeft");
    expect(await ports(page)).toEqual([0xFF, 0xA9, 0x20]);
    await page.keyboard.up("ArrowLeft");
    await page.keyboard.down("ArrowRight");
    expect(await ports(page)).toEqual([0xFF, 0xC9, 0x40]);
    await page.keyboard.up("ArrowRight");
    expect(await ports(page)).toEqual([0xFF, 0x89, 0x00]);
  });

  test("AD maps to the same joystick bits", async ({ page }) => {
    await page.keyboard.down("KeyA");
    expect(await ports(page)).toEqual([0xFF, 0xA9, 0x20]);
    await page.keyboard.up("KeyA");
    await page.keyboard.down("KeyD");
    expect(await ports(page)).toEqual([0xFF, 0xC9, 0x40]);
    await page.keyboard.up("KeyD");
  });

  test("Z / Space fire sets bit 0x10", async ({ page }) => {
    await page.keyboard.down("KeyZ");
    expect(await ports(page)).toEqual([0xFF, 0x99, 0x10]);
    await page.keyboard.up("KeyZ");
    await page.keyboard.down("Space");
    expect(await ports(page)).toEqual([0xFF, 0x99, 0x10]);
    await page.keyboard.up("Space");
    expect(await ports(page)).toEqual([0xFF, 0x89, 0x00]);
  });

  test("5 inserts a coin by pulling IN1 bit 0 low", async ({ page }) => {
    await page.keyboard.down("Digit5");
    expect(await ports(page)).toEqual([0xFF, 0x88, 0x00]);
    await page.keyboard.up("Digit5");
    expect(await ports(page)).toEqual([0xFF, 0x89, 0x00]);
  });

  test("1 and 2 start 1P/2P (IN1 bits 0x04/0x02)", async ({ page }) => {
    await page.keyboard.down("Digit1");
    expect(await ports(page)).toEqual([0xFF, 0x8D, 0x00]);
    await page.keyboard.up("Digit1");
    await page.keyboard.down("Digit2");
    expect(await ports(page)).toEqual([0xFF, 0x8B, 0x00]);
    await page.keyboard.up("Digit2");
    expect(await ports(page)).toEqual([0xFF, 0x89, 0x00]);
  });

  test("numpad 5/1/2 map to the same coin and start bits", async ({ page }) => {
    await page.keyboard.down("Numpad5");
    expect((await ports(page))[1]).toBe(0x88);
    await page.keyboard.up("Numpad5");
    await page.keyboard.down("Numpad1");
    expect((await ports(page))[1]).toBe(0x8D);
    await page.keyboard.up("Numpad1");
    await page.keyboard.down("Numpad2");
    expect((await ports(page))[1]).toBe(0x8B);
    await page.keyboard.up("Numpad2");
  });
});
