import { test, expect, Page } from "@playwright/test";

// Touch control panel (shared/arcade-touch.js): shown only on a touch screen with no mouse, a
// 4-way restrictor like the cabinet's stick, and the same IN0 bits the keyboard drives.

const in0 = (page: Page) => page.evaluate(() => (window as any).__test.machine.in0());
const IDLE = { in0: 0xFF, in1: 0xFF };
const ports = (page: Page) =>
  page.evaluate(() => { const m = (window as any).__test.machine; return { in0: m.in0(), in1: m.in1() }; });
const expected = (over: Record<string, number> = {}) => ({ ...IDLE, ...over });

async function dragStick(page: Page, dx: number, dy: number) {
  const box = (await page.locator("#touchControls .touch-gate").boundingBox())!;
  const cx = box.x + box.width / 2;
  const cy = box.y + box.height / 2;
  await page.mouse.move(cx, cy);
  await page.mouse.down();
  await page.mouse.move(cx + dx * box.width / 2, cy + dy * box.height / 2);
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

  test("the panel shows, with the monitor filling the width in portrait", async ({ page }) => {
    await expect(page.locator("#touchControls .touch-stick")).toBeVisible();
    await expect(page.locator("#touchControls .touch-btn")).toHaveCount(0);
    const bezel = (await page.locator("#bezel").boundingBox())!;
    const card = (await page.locator("#displayCard").boundingBox())!;
    expect(bezel.width).toBeGreaterThan(card.width - 24);
  });

  const cases: [string, number, number, number][] = [
    ["up", 0, -1, 0x01], ["left", -1, 0, 0x02], ["right", 1, 0, 0x04], ["down", 0, 1, 0x08],
  ];
  for (const [name, dx, dy, bit] of cases) {
    test(`pushing the stick ${name} clears IN0 bit 0x${bit.toString(16)} until let go`, async ({ page }) => {
      await dragStick(page, dx * 0.9, dy * 0.9);
      expect(await in0(page)).toBe(0xFF & ~bit);
      await page.mouse.up();
      expect(await in0(page)).toBe(0xFF);
    });
  }

  test("a diagonal push closes one switch, never two", async ({ page }) => {
    await dragStick(page, 0.7, -0.6);
    expect(await in0(page)).toBe(0xFF & ~0x04);
    await page.mouse.up();
  });

  test("the held direction sticks through a near-diagonal until the other axis wins", async ({ page }) => {
    await dragStick(page, 0, -0.9);
    expect(await in0(page)).toBe(0xFF & ~0x01);
    const box = (await page.locator("#touchControls .touch-gate").boundingBox())!;
    const cx = box.x + box.width / 2;
    const cy = box.y + box.height / 2;
    await page.mouse.move(cx + 0.7 * box.width / 2, cy - 0.6 * box.height / 2);
    expect(await in0(page)).toBe(0xFF & ~0x01);
    await page.mouse.move(cx + 0.9 * box.width / 2, cy - 0.2 * box.height / 2);
    expect(await in0(page)).toBe(0xFF & ~0x04);
    await page.mouse.up();
  });

  test("a small nudge inside the dead zone does nothing", async ({ page }) => {
    await dragStick(page, 0.1, 0.1);
    expect(await in0(page)).toBe(0xFF);
    await page.mouse.up();
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

  test("a finger holds the input until it lifts, with a buzz", async ({ page }) => {
    const a = await spot(page, `.touch-gate`, 0, 0);
    await touch(page, "touchStart", [a]);
    await touch(page, "touchMove", [await spot(page, `.touch-gate`, 0, -0.9)]);
    expect(await ports(page)).toEqual(expected({ in0: 0xFF & ~0x01 }));
    expect(await page.evaluate(() => (window as any).__buzz)).toBeGreaterThan(0);
    await touch(page, "touchEnd", []);
    expect(await ports(page)).toEqual(IDLE);
  });

  test("a real key press mid-hold lets go of everything", async ({ page }) => {
    await touch(page, "touchStart", [await spot(page, `.touch-gate`, 0, -0.9)]);
    expect(await ports(page)).toEqual(expected({ in0: 0xFF & ~0x01 }));
    await page.keyboard.press("KeyQ");
    await expect(page.locator("#touchControls")).toBeHidden();
    expect(await ports(page)).toEqual(IDLE);
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
