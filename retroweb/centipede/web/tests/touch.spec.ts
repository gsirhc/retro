import { test, expect, Page } from "@playwright/test";

// Touch control panel (shared/arcade-touch.js): shown only on a touch screen with no mouse. The
// stick rolls the trackball the way the arrow keys do, and the fire button is IN1 bit 0x04.

const IDLE_IN1 = 0xff;
const in1 = (page: Page) => page.evaluate(() => (window as any).__test.machine.in1());
const track = (page: Page) =>
  page.evaluate(() => { const m = (window as any).__test.machine; return { x: m.trackX(), y: m.trackY() }; });

async function dragStick(page: Page, dx: number, dy: number) {
  const box = (await page.locator("#touchControls .touch-gate").boundingBox())!;
  const cx = box.x + box.width / 2;
  const cy = box.y + box.height / 2;
  await page.mouse.move(cx, cy);
  await page.mouse.down();
  await page.mouse.move(cx + dx * box.width / 2, cy + dy * box.height / 2);
}

async function pressFire(page: Page) {
  const box = (await page.locator(`#touchControls .touch-btn[data-code="Space"] .touch-btn-cap`).boundingBox())!;
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  await page.mouse.down();
}

test.describe("desktop", () => {
  test("touch controls stay hidden with a mouse and keyboard", async ({ page }) => {
    await page.goto("/?test=1");
    await expect(page.locator("#touchControls")).toBeHidden();
    await expect(page.locator("html")).not.toHaveClass(/arcade-touch/);
  });
});

test.describe("touch screen", () => {
  test.use({ hasTouch: true, isMobile: true, viewport: { width: 390, height: 844 } });

  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await page.waitForFunction(() => !!(window as any).__test?.machine);
  });

  test("the panel shows a stick and a fire button, with the monitor filling the width in portrait", async ({ page }) => {
    await expect(page.locator("#touchControls .touch-stick")).toBeVisible();
    await expect(page.locator("#touchControls .touch-btn")).toHaveCount(1);
    const bezel = (await page.locator("#bezel").boundingBox())!;
    const card = (await page.locator("#displayCard").boundingBox())!;
    expect(bezel.width).toBeGreaterThan(card.width - 24);
  });

  const cases: [string, number, number, "x" | "y", number][] = [
    ["right", 0.9, 0, "x", 0x80],
    ["left", -0.9, 0, "x", 0],
    ["up", 0, -0.9, "y", 0x80],
    ["down", 0, 0.9, "y", 0],
  ];
  for (const [name, dx, dy, axis, sign] of cases) {
    test(`pushing the stick ${name} rolls the ${axis} counter with sign 0x${sign.toString(16)}`, async ({ page }) => {
      await dragStick(page, dx, dy);
      await expect.poll(async () => (await track(page))[axis] & 0x0f).not.toBe(0);
      expect((await track(page))[axis] & 0x80).toBe(sign);
      await page.mouse.up();
      const held = (await track(page))[axis];
      await page.waitForTimeout(150);
      expect((await track(page))[axis]).toBe(held);
    });
  }

  test("a small nudge inside the dead zone does nothing", async ({ page }) => {
    await dragStick(page, 0.1, 0.1);
    await page.waitForTimeout(200);
    expect(await track(page)).toEqual({ x: 0, y: 0 });
    await page.mouse.up();
  });

  test("the fire button clears IN1 bit 0x04 while held", async ({ page }) => {
    await pressFire(page);
    expect(await in1(page)).toBe(0xfb);
    await page.mouse.up();
    expect(await in1(page)).toBe(IDLE_IN1);
  });

  test("every control sits fully on screen", async ({ page }) => {
    const vw = page.viewportSize()!.width;
    for (const box of await page.locator("#touchControls .touch-gate, #touchControls .touch-btn").evaluateAll(
      (els) => els.map((e) => e.getBoundingClientRect().toJSON()))) {
      expect(box.left).toBeGreaterThanOrEqual(0);
      expect(box.right).toBeLessThanOrEqual(vw);
    }
  });

  test("a real key press hides the panel for the visit", async ({ page }) => {
    await page.keyboard.press("ArrowUp");
    await expect(page.locator("#touchControls")).toBeHidden();
    await expect(page.locator("html")).not.toHaveClass(/arcade-touch/);
  });
});

// Real touch events through CDP, as a phone sends them, several fingers at once.
async function spot(page: Page, sel: string, dx: number, dy: number) {
  const b = (await page.locator(`#touchControls ${sel}`).boundingBox())!;
  return { x: b.x + b.width / 2 + dx * b.width / 2, y: b.y + b.height / 2 + dy * b.height / 2, id: 0 };
}
// One CDP session per page: a touch sequence doesn't carry across sessions.
const sessions = new WeakMap<Page, any>();
async function touch(page: Page, type: string, points: { x: number; y: number; id: number }[]) {
  if (!sessions.has(page)) sessions.set(page, await page.context().newCDPSession(page));
  await sessions.get(page).send("Input.dispatchTouchEvent", { type, touchPoints: points });
}

test.describe("real touch", () => {
  test.use({ hasTouch: true, isMobile: true, viewport: { width: 390, height: 844 } });

  test.beforeEach(async ({ page }) => {
    await page.addInitScript(() => {
      Object.defineProperty(Navigator.prototype, "vibrate", {
        configurable: true,
        value: () => { (window as any).__buzz = ((window as any).__buzz || 0) + 1; return true; },
      });
    });
    await page.goto("/?test=1");
    await page.waitForFunction(() => !!(window as any).__test?.machine);
  });

  test("a finger on the stick rolls the ball until it lifts, with a buzz", async ({ page }) => {
    await touch(page, "touchStart", [await spot(page, ".touch-gate", 0, 0)]);
    await touch(page, "touchMove", [await spot(page, ".touch-gate", 0.9, 0)]);
    await expect.poll(async () => (await track(page)).x & 0x0f).not.toBe(0);
    expect((await track(page)).x & 0x80).toBe(0x80);
    expect(await page.evaluate(() => (window as any).__buzz)).toBeGreaterThan(0);
    await touch(page, "touchEnd", []);
  });

  test("two fingers at once: stick and fire are held together", async ({ page }) => {
    const a = await spot(page, ".touch-gate", 0.9, 0);
    const b = await spot(page, `.touch-btn[data-code="Space"] .touch-btn-cap`, 0, 0);
    await touch(page, "touchStart", [{ ...a, x: (await spot(page, ".touch-gate", 0, 0)).x, y: (await spot(page, ".touch-gate", 0, 0)).y }]);
    await touch(page, "touchMove", [a]);
    await touch(page, "touchStart", [a, { ...b, id: 1 }]);
    expect(await in1(page)).toBe(0xfb);
    await expect.poll(async () => (await track(page)).x & 0x0f).not.toBe(0);
    await touch(page, "touchEnd", []);
    expect(await in1(page)).toBe(IDLE_IN1);
  });

  test("a real key press mid-hold lets go of everything", async ({ page }) => {
    await touch(page, "touchStart", [await spot(page, `.touch-btn[data-code="Space"] .touch-btn-cap`, 0, 0)]);
    expect(await in1(page)).toBe(0xfb);
    await page.keyboard.press("KeyQ");
    await expect(page.locator("#touchControls")).toBeHidden();
    expect(await in1(page)).toBe(IDLE_IN1);
    await touch(page, "touchEnd", []);
  });
});

test.describe("landscape", () => {
  test.use({ hasTouch: true, isMobile: true, viewport: { width: 844, height: 390 } });

  test("the monitor keeps its height instead of filling the width", async ({ page }) => {
    await page.goto("/?test=1");
    await expect(page.locator("#touchControls")).toBeVisible();
    const bezel = (await page.locator("#bezel").boundingBox())!;
    const card = (await page.locator("#displayCard").boundingBox())!;
    expect(bezel.width).toBeLessThan(card.width - 24);
  });
});
