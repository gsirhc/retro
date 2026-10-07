import { test, expect } from "@playwright/test";

// IN0/IN1 are active-high: released reads 0, pressed reads 1.

test.describe("keyboard input", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await page.locator("#screen").click();
  });

  const in0 = (page: import("@playwright/test").Page) =>
    page.evaluate(() => (window as any).__test.machine.in0());
  const in1 = (page: import("@playwright/test").Page) =>
    page.evaluate(() => (window as any).__test.machine.in1());

  test("released state reads back 0 on both ports", async ({ page }) => {
    expect(await in0(page)).toBe(0);
    expect(await in1(page)).toBe(0);
  });

  test("Shift sets IN0 bit 0x08 (hyperspace) while held", async ({ page }) => {
    await page.keyboard.down("ShiftLeft");
    expect(await in0(page)).toBe(0x08);
    await page.keyboard.up("ShiftLeft");
    expect(await in0(page)).toBe(0);
  });

  test("Z and Space both set IN0 bit 0x10 (fire)", async ({ page }) => {
    await page.keyboard.down("KeyZ");
    expect(await in0(page)).toBe(0x10);
    await page.keyboard.up("KeyZ");
    expect(await in0(page)).toBe(0);

    await page.keyboard.down("Space");
    expect(await in0(page)).toBe(0x10);
    await page.keyboard.up("Space");
    expect(await in0(page)).toBe(0);
  });

  test("Space does not scroll the page", async ({ page }) => {
    await page.evaluate(() => { document.body.style.minHeight = "4000px"; });
    await page.evaluate(() => window.scrollTo(0, 0));
    await page.keyboard.down("Space");
    await page.keyboard.up("Space");
    expect(await page.evaluate(() => window.scrollY)).toBe(0);
  });

  const in1Cases: [string, number][] = [
    ["Digit5", 0x01], // left coin
    ["Digit6", 0x02], // center coin
    ["Digit7", 0x04], // right coin
    ["Digit1", 0x08], // 1P start
    ["Digit2", 0x10], // 2P start
    ["ArrowUp", 0x20], // thrust
    ["ArrowRight", 0x40], // rotate right
    ["ArrowLeft", 0x80], // rotate left
  ];
  for (const [key, bit] of in1Cases) {
    test(`${key} sets IN1 bit 0x${bit.toString(16)} while held`, async ({ page }) => {
      await page.keyboard.down(key);
      expect(await in1(page)).toBe(bit);
      await page.keyboard.up(key);
      expect(await in1(page)).toBe(0);
    });
  }

  test("WASD maps to the same thrust/rotate bits as the arrow keys", async ({ page }) => {
    await page.keyboard.down("KeyW");
    expect(await in1(page)).toBe(0x20);
    await page.keyboard.up("KeyW");
    await page.keyboard.down("KeyD");
    expect(await in1(page)).toBe(0x40);
    await page.keyboard.up("KeyD");
    await page.keyboard.down("KeyA");
    expect(await in1(page)).toBe(0x80);
    await page.keyboard.up("KeyA");
  });

  test("numpad 5/6/7/1/2 map to the same coin and start bits", async ({ page }) => {
    await page.keyboard.down("Numpad5");
    expect(await in1(page)).toBe(0x01);
    await page.keyboard.up("Numpad5");

    await page.keyboard.down("Numpad1");
    expect(await in1(page)).toBe(0x08);
    await page.keyboard.up("Numpad1");

    await page.keyboard.down("Numpad2");
    expect(await in1(page)).toBe(0x10);
    await page.keyboard.up("Numpad2");
  });
});
