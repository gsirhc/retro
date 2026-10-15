import { test, expect } from "@playwright/test";
import { frames } from "./helpers";

// IN1 is active-low (idle 0xFF). The trackball is a 4-bit counter plus a sign bit, one step per 1/60 s while a key is held.

test.describe("keyboard input", () => {
  test.beforeEach(async ({ page }) => {
    // Linux headless Chromium grants pointer lock on click; macOS denies it.
    await page.addInitScript(() => { Element.prototype.requestPointerLock = () => Promise.resolve(); });
    await page.goto("/?test=1");
    await page.waitForFunction(() => !!(window as any).__test?.machine);
    await page.locator("#screen").click();
  });

  const in1 = (page: import("@playwright/test").Page) =>
    page.evaluate(() => (window as any).__test.machine.in1());
  const trackX = (page: import("@playwright/test").Page) =>
    page.evaluate(() => (window as any).__test.machine.trackX());
  const trackY = (page: import("@playwright/test").Page) =>
    page.evaluate(() => (window as any).__test.machine.trackY());
  const pos = (page: import("@playwright/test").Page) =>
    page.evaluate(() => (window as any).__test.trackPos);

  test("released state reads idle: IN1 0xFF, trackball 0, service high", async ({ page }) => {
    expect(await in1(page)).toBe(0xff);
    expect(await trackX(page)).toBe(0);
    expect(await trackY(page)).toBe(0);
    expect(await page.evaluate(() => (window as any).__test.in0Switches)).toBe(0x20);
  });

  test("Z and Space both clear IN1 bit 0x04 (fire)", async ({ page }) => {
    await page.keyboard.down("KeyZ");
    expect(await in1(page)).toBe(0xfb);
    await page.keyboard.up("KeyZ");
    expect(await in1(page)).toBe(0xff);

    await page.keyboard.down("Space");
    expect(await in1(page)).toBe(0xfb);
    await page.keyboard.up("Space");
    expect(await in1(page)).toBe(0xff);
  });

  test("Space does not scroll the page", async ({ page }) => {
    await page.evaluate(() => { document.body.style.minHeight = "4000px"; });
    await page.evaluate(() => window.scrollTo(0, 0));
    await page.keyboard.down("Space");
    await page.keyboard.up("Space");
    expect(await page.evaluate(() => window.scrollY)).toBe(0);
  });

  const in1Cases: [string, number][] = [
    ["Digit1", 0xfe], // 1P start
    ["Digit2", 0xfd], // 2P start
    ["Digit5", 0xdf], // coin 1
    ["Digit6", 0xbf], // coin 2
  ];
  for (const [key, value] of in1Cases) {
    test(`${key} reads IN1 = 0x${value.toString(16)} while held`, async ({ page }) => {
      await page.keyboard.down(key);
      expect(await in1(page)).toBe(value);
      await page.keyboard.up(key);
      expect(await in1(page)).toBe(0xff);
    });
  }

  test("numpad 5/6/1/2 map to the same coin and start bits", async ({ page }) => {
    for (const [key, value] of [["Numpad5", 0xdf], ["Numpad6", 0xbf], ["Numpad1", 0xfe], ["Numpad2", 0xfd]] as [string, number][]) {
      await page.keyboard.down(key);
      expect(await in1(page)).toBe(value);
      await page.keyboard.up(key);
    }
    expect(await in1(page)).toBe(0xff);
  });

  // X is wired reversed (MAME TRACK0_X PORT_REVERSE): right counts down, so the sign latches set.
  const trackCases: [string, "x" | "y", number][] = [
    ["ArrowLeft", "x", 0x00],
    ["ArrowRight", "x", 0x80],
    ["ArrowDown", "y", 0x00],
    ["ArrowUp", "y", 0x80],
    ["KeyA", "x", 0x00],
    ["KeyD", "x", 0x80],
    ["KeyS", "y", 0x00],
    ["KeyW", "y", 0x80],
  ];
  for (const [key, axis, sign] of trackCases) {
    test(`${key} rolls the trackball ${axis} counter with sign 0x${sign.toString(16).padStart(2, "0")}`, async ({ page }) => {
      await page.keyboard.down(key);
      await expect.poll(async () => (await pos(page))[axis]).not.toBe(0);
      const port = axis === "x" ? trackX : trackY;
      expect((await port(page)) & 0x80).toBe(sign);
      const other = axis === "x" ? "y" : "x";
      expect((await pos(page))[other]).toBe(0);
      await page.keyboard.up(key);
      expect(await in1(page)).toBe(0xff);
    });
  }

  test("the counter steps by 2 and holds its value once the key is up", async ({ page }) => {
    await page.keyboard.down("ArrowLeft");
    await expect.poll(async () => (await pos(page)).x).toBeGreaterThan(0);
    await page.keyboard.up("ArrowLeft");
    const a = await pos(page);
    expect(a.x).toBeGreaterThan(0);
    expect(a.x % 2).toBe(0);
    expect(await trackX(page)).toBe(a.x & 0x0f);
    await frames(page, 9);
    expect(await pos(page)).toEqual(a);
  });

  test("the sign bit keeps the last direction after release", async ({ page }) => {
    await page.keyboard.down("ArrowRight");
    await expect.poll(() => trackX(page)).toBeGreaterThanOrEqual(0x80);
    await page.keyboard.up("ArrowRight");
    await frames(page, 6);
    expect((await trackX(page)) & 0x80).toBe(0x80);
  });

  test("dragging the mouse on the screen rolls the trackball", async ({ page }) => {
    const box = await page.locator("#screen").boundingBox();
    expect(box).toBeTruthy();
    const x0 = await trackX(page);
    await page.mouse.move(box!.x + box!.width / 2, box!.y + box!.height / 2);
    await page.mouse.down();
    await page.mouse.move(box!.x + box!.width / 2 + 40, box!.y + box!.height / 2, { steps: 8 });
    await page.mouse.up();
    await expect.poll(async () => (await trackX(page)) & 0x0f).not.toBe(x0 & 0x0f);
  });
});
