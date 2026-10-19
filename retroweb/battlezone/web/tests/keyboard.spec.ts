import { test, expect, type Page } from "@playwright/test";

// IN0 is active low (released 0x3f), IN3 is active high (released 0).

test.describe("keyboard input", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await page.waitForFunction(() => !!(window as any).__test?.machine);
    await page.locator("#screen").click();
  });

  const in0 = (page: Page) => page.evaluate(() => (window as any).__test.machine.in0());
  const in3 = (page: Page) => page.evaluate(() => (window as any).__test.machine.in3());

  test("released state reads 0x3f on IN0 and 0 on IN3", async ({ page }) => {
    expect(await in0(page)).toBe(0x3f);
    expect(await in3(page)).toBe(0);
  });

  const in0Cases: [string, number][] = [
    ["Digit5", 0x3d],
    ["Numpad5", 0x3d],
    ["Digit6", 0x3e],
    ["Numpad6", 0x3e],
    ["F1", 0x1f],
  ];
  for (const [key, value] of in0Cases) {
    test(`${key} drives IN0 to 0x${value.toString(16)} while held`, async ({ page }) => {
      await page.keyboard.down(key);
      expect(await in0(page)).toBe(value);
      await page.keyboard.up(key);
      expect(await in0(page)).toBe(0x3f);
    });
  }

  const in3Cases: [string, number][] = [
    ["KeyW", 0x08],
    ["KeyS", 0x04],
    ["KeyI", 0x02],
    ["KeyK", 0x01],
    ["Space", 0x10],
    ["KeyZ", 0x10],
    ["ControlLeft", 0x10],
    ["Digit1", 0x20],
    ["Numpad1", 0x20],
    ["ArrowUp", 0x0a],
    ["ArrowDown", 0x05],
    ["ArrowLeft", 0x06],
    ["ArrowRight", 0x09],
  ];
  for (const [key, bits] of in3Cases) {
    test(`${key} sets IN3 0x${bits.toString(16)} while held`, async ({ page }) => {
      await page.keyboard.down(key);
      expect(await in3(page)).toBe(bits);
      await page.keyboard.up(key);
      expect(await in3(page)).toBe(0);
    });
  }

  test("Digit2 does nothing, there is one START button", async ({ page }) => {
    await page.keyboard.down("Digit2");
    expect(await in3(page)).toBe(0);
    expect(await in0(page)).toBe(0x3f);
    await page.keyboard.up("Digit2");
  });

  test("pushing one lever both ways cancels it", async ({ page }) => {
    await page.keyboard.down("KeyW");
    await page.keyboard.down("KeyS");
    expect((await in3(page)) & 0x0c).toBe(0);
    await page.keyboard.up("KeyS");
    expect(await in3(page)).toBe(0x08);
    await page.keyboard.up("KeyW");

    await page.keyboard.down("KeyI");
    await page.keyboard.down("KeyK");
    expect((await in3(page)) & 0x03).toBe(0);
    await page.keyboard.up("KeyK");
    await page.keyboard.up("KeyI");
    expect(await in3(page)).toBe(0);
  });

  test("the levers are independent of each other", async ({ page }) => {
    await page.keyboard.down("KeyW");
    await page.keyboard.down("KeyK");
    expect(await in3(page)).toBe(0x09);
    await page.keyboard.up("KeyW");
    await page.keyboard.up("KeyK");
  });

  test("Space and the arrow keys do not scroll the page", async ({ page }) => {
    await page.evaluate(() => { document.body.style.minHeight = "4000px"; });
    await page.evaluate(() => window.scrollTo(0, 0));
    for (const key of ["Space", "ArrowDown", "ArrowUp"]) {
      await page.keyboard.down(key);
      await page.keyboard.up(key);
    }
    expect(await page.evaluate(() => window.scrollY)).toBe(0);
  });
});
