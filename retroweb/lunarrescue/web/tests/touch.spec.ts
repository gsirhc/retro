import { test, expect, Page } from "@playwright/test";

// Touch control panel (shared/arcade-touch.js): shown only on a touch screen with no mouse,
// driving the same port bits the keyboard does.

const IDLE = { in0: 0xFF, in1: 0x89, in2: 0x00 };
// Upright PCB: the P1 controls sit on bits 4-6 of all three ports.
const ctl = (bits: number) => ({ in0: 0xFF, in1: 0x89 | bits, in2: bits });
const ports = (page: Page) =>
  page.evaluate(() => { const m = (window as any).__test.machine; return { in0: m.in0(), in1: m.in1(), in2: m.in2() }; });

async function dragStick(page: Page, dx: number, dy: number) {
  const box = (await page.locator("#touchControls .touch-gate").boundingBox())!;
  const cx = box.x + box.width / 2;
  const cy = box.y + box.height / 2;
  await page.mouse.move(cx, cy);
  await page.mouse.down();
  await page.mouse.move(cx + dx * box.width / 2, cy + dy * box.height / 2);
}

async function pressButton(page: Page, code: string) {
  const box = (await page.locator(`#touchControls .touch-btn[data-code="${code}"] .touch-btn-cap`).boundingBox())!;
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

  test("the panel shows, with the monitor filling the width in portrait", async ({ page }) => {
    await expect(page.locator("#touchControls .touch-stick")).toBeVisible();
    await expect(page.locator("#touchControls .touch-btn")).toHaveCount(1);
    const bezel = (await page.locator("#bezel").boundingBox())!;
    const card = (await page.locator("#displayCard").boundingBox())!;
    expect(bezel.width).toBeGreaterThan(card.width - 24);
  });

  test("pushing the stick left sets bit 0x20 until let go", async ({ page }) => {
    await dragStick(page, -0.9, 0);
    expect(await ports(page)).toEqual(ctl(0x20));
    await page.mouse.up();
    expect(await ports(page)).toEqual(IDLE);
  });

  test("pushing the stick right sets bit 0x40 until let go", async ({ page }) => {
    await dragStick(page, 0.9, 0);
    expect(await ports(page)).toEqual(ctl(0x40));
    await page.mouse.up();
    expect(await ports(page)).toEqual(IDLE);
  });

  test("vertical pushes are ignored, a diagonal reads as its horizontal part", async ({ page }) => {
    await dragStick(page, 0, -0.9);
    expect(await ports(page)).toEqual(IDLE);
    await page.mouse.up();
    await dragStick(page, 0.7, -0.7);
    expect(await ports(page)).toEqual(ctl(0x40));
    await page.mouse.up();
  });

  test("a small nudge inside the dead zone does nothing", async ({ page }) => {
    await dragStick(page, 0.1, 0.1);
    expect(await ports(page)).toEqual(IDLE);
    await page.mouse.up();
  });

  test("the fire button sets bit 0x10 while held", async ({ page }) => {
    await pressButton(page, "Space");
    expect(await ports(page)).toEqual(ctl(0x10));
    await page.mouse.up();
    expect(await ports(page)).toEqual(IDLE);
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
    await touch(page, "touchMove", [await spot(page, `.touch-gate`, -0.9, 0)]);
    expect(await ports(page)).toEqual(ctl(0x20));
    expect(await page.evaluate(() => (window as any).__buzz)).toBeGreaterThan(0);
    await touch(page, "touchEnd", []);
    expect(await ports(page)).toEqual(IDLE);
  });

  test("two fingers at once: both inputs are held together", async ({ page }) => {
    const a = await spot(page, `.touch-gate`, -0.9, 0);
    const b = await spot(page, `.touch-btn[data-code="Space"] .touch-btn-cap`, 0, 0);
    await touch(page, "touchStart", [a]);
    await touch(page, "touchStart", [a, { ...b, id: 1 }]);
    expect(await ports(page)).toEqual(ctl(0x20 | 0x10));
    await touch(page, "touchEnd", []);
    expect(await ports(page)).toEqual(IDLE);
  });

  test("a real key press mid-hold lets go of everything", async ({ page }) => {
    await touch(page, "touchStart", [await spot(page, `.touch-gate`, -0.9, 0)]);
    expect(await ports(page)).toEqual(ctl(0x20));
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
