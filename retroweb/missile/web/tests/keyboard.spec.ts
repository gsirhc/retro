import { test, expect, type Page } from "@playwright/test";
import { frames } from "./helpers";

// IN0 and IN1 are active-low (idle 0xFF / 0x67). The trackball is two 4-bit counters, 2 counts per 1/60 s while a key is held.

test.describe("keyboard input", () => {
  test.beforeEach(async ({ page }) => {
    // Linux headless Chromium grants pointer lock on click; macOS denies it.
    await page.addInitScript(() => { Element.prototype.requestPointerLock = () => Promise.resolve(); });
    await page.goto("/?test=1");
    await page.waitForFunction(() => !!(window as any).__test?.machine);
    await page.locator("#screen").click();
  });

  const m = (page: Page, fn: string) => page.evaluate((f) => (window as any).__test.machine[f](), fn);
  const in0 = (page: Page) => m(page, "in0");
  const in1 = (page: Page) => m(page, "in1");
  const trackX = (page: Page) => m(page, "trackX");
  const trackY = (page: Page) => m(page, "trackY");
  const pos = (page: Page) => page.evaluate(() => (window as any).__test.trackPos);
  // The counters wrap every 8 frames, so a polled read can't tell direction; log each nudge.
  const recordTrack = (page: Page) => page.evaluate(() => {
    const t = (window as any).__test;
    const log: { x: number; y: number }[] = [];
    (window as any).__trackLog = log;
    const setY = t.machine.setTrackY.bind(t.machine);
    t.machine.setTrackY = (v: number) => { setY(v); log.push({ ...t.trackPos }); };
  });
  const trackLog = (page: Page) => page.evaluate(() => (window as any).__trackLog as { x: number; y: number }[]);

  test("released state reads idle: IN0 0xFF, IN1 0x67, trackball 0", async ({ page }) => {
    expect(await in0(page)).toBe(0xff);
    expect(await in1(page)).toBe(0x67);
    expect(await trackX(page)).toBe(0);
    expect(await trackY(page)).toBe(0);
  });

  const fireCases: [string, number][] = [
    ["KeyZ", 0x63], // left launcher, bit 2
    ["KeyX", 0x65], // center, bit 1
    ["KeyC", 0x66], // right, bit 0
    ["KeyT", 0x47], // slam, bit 5
  ];
  for (const [key, value] of fireCases) {
    test(`${key} reads IN1 = 0x${value.toString(16)} while held`, async ({ page }) => {
      await page.keyboard.down(key);
      expect(await in1(page)).toBe(value);
      await page.keyboard.up(key);
      expect(await in1(page)).toBe(0x67);
    });
  }

  test("all three launchers fire together", async ({ page }) => {
    await page.keyboard.down("KeyZ");
    await page.keyboard.down("KeyX");
    await page.keyboard.down("KeyC");
    expect(await in1(page)).toBe(0x60);
    await page.keyboard.up("KeyZ");
    await page.keyboard.up("KeyX");
    await page.keyboard.up("KeyC");
    expect(await in1(page)).toBe(0x67);
  });

  const in0Cases: [string, number][] = [
    ["Digit1", 0xef], // 1P start
    ["Digit2", 0xf7], // 2P start
    ["Digit5", 0xdf], // left coin
    ["Digit6", 0x7f], // right coin
    ["Numpad1", 0xef],
    ["Numpad2", 0xf7],
    ["Numpad5", 0xdf],
    ["Numpad6", 0x7f],
  ];
  for (const [key, value] of in0Cases) {
    test(`${key} reads IN0 = 0x${value.toString(16)} while held`, async ({ page }) => {
      await page.keyboard.down(key);
      expect(await in0(page)).toBe(value);
      await page.keyboard.up(key);
      expect(await in0(page)).toBe(0xff);
    });
  }

  // Upward roll counts Y up (rev 3 IRQ, $7BE6). Right counts X up.
  const trackCases: [string, "x" | "y", number][] = [
    ["ArrowRight", "x", 1],
    ["ArrowLeft", "x", -1],
    ["ArrowUp", "y", 1],
    ["ArrowDown", "y", -1],
    ["KeyD", "x", 1],
    ["KeyA", "x", -1],
    ["KeyW", "y", 1],
    ["KeyS", "y", -1],
  ];
  for (const [key, axis, dir] of trackCases) {
    test(`${key} counts the ${axis} counter ${dir > 0 ? "up" : "down"}`, async ({ page }) => {
      await recordTrack(page);
      await page.keyboard.down(key);
      await expect.poll(async () => (await trackLog(page)).length).toBeGreaterThan(0);
      await page.keyboard.up(key);
      const log = await trackLog(page);
      expect(log[0][axis]).toBe(dir > 0 ? 2 : 14);
      const other = axis === "x" ? "y" : "x";
      for (const p of log) {
        expect(p[axis] % 2).toBe(0);
        expect(p[other]).toBe(0);
      }
      const p = await pos(page);
      expect(await (axis === "x" ? trackX : trackY)(page)).toBe(p[axis]);
      expect(await (axis === "x" ? trackY : trackX)(page)).toBe(0);
    });
  }

  test("the counter wraps at 4 bits and holds its value once the key is up", async ({ page }) => {
    await page.keyboard.down("ArrowRight");
    await page.waitForFunction(() => {
      const w = window as any;
      const x = w.__test.trackPos.x;
      w.__wrapSteps = (w.__wrapSteps ?? 0) + ((x - (w.__wrapPrev ?? x)) & 0x0f);
      w.__wrapPrev = x;
      return w.__wrapSteps >= 16;
    });
    await page.keyboard.up("ArrowRight");
    const a = await pos(page);
    expect(a.x).toBeLessThan(16);
    await frames(page, 9);
    expect(await pos(page)).toEqual(a);
    expect(await trackX(page)).toBe(a.x);
  });

  test("dragging the mouse on the screen rolls the trackball", async ({ page }) => {
    const box = await page.locator("#screen").boundingBox();
    expect(box).toBeTruthy();
    const x0 = await trackX(page);
    await page.mouse.move(box!.x + box!.width / 2, box!.y + box!.height / 2);
    await page.mouse.down();
    await page.mouse.move(box!.x + box!.width / 2 + 40, box!.y + box!.height / 2, { steps: 8 });
    await page.mouse.up();
    await expect.poll(() => trackX(page)).not.toBe(x0);
    expect(await trackY(page)).toBe(0);
  });

  test("mouse buttons do not fire until the pointer is captured", async ({ page }) => {
    await page.locator("#screen").dispatchEvent("mousedown", { button: 2 });
    expect(await in1(page)).toBe(0x67);
  });

  test("with the pointer captured, left / middle / right buttons fire the three launchers", async ({ page }) => {
    // Headless Chromium denies pointer lock, so fake the captured state.
    await page.evaluate(() => {
      const screen = document.getElementById("screen");
      Object.defineProperty(document, "pointerLockElement", { get: () => screen, configurable: true });
    });
    const cases: [number, number][] = [[0, 0x63], [1, 0x65], [2, 0x66]];
    for (const [button, value] of cases) {
      await page.locator("#screen").dispatchEvent("mousedown", { button });
      expect(await in1(page)).toBe(value);
      await page.evaluate((b) => window.dispatchEvent(new MouseEvent("mouseup", { button: b })), button);
      expect(await in1(page)).toBe(0x67);
    }
  });

  test("the context menu is suppressed on the screen", async ({ page }) => {
    const prevented = await page.locator("#screen").evaluate((el) => {
      const e = new MouseEvent("contextmenu", { bubbles: true, cancelable: true });
      el.dispatchEvent(e);
      return e.defaultPrevented;
    });
    expect(prevented).toBe(true);
  });
});
