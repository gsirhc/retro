import { test, expect } from "./fixtures";
import { bootLive } from "./helpers";

// Mouse capture is off for a first visit. Enable Mouse is remembered in retro8080.pc486.ui;
// Pointer Lock still needs a canvas click.
test.describe("PS/2 mouse", () => {
  test("capture checkbox is unchecked by default", async ({ livePage: page }) => {
    await expect(page.locator("#mouseCaptureEnabled")).not.toBeChecked();
  });

  test("Enable Mouse preference persists across reload", async ({ page }) => {
    await bootLive(page);
    await page.evaluate(() => localStorage.removeItem("retro8080.pc486.ui"));
    await page.locator("#mouseCaptureEnabled").check();
    await expect(page.locator("#mouseCaptureEnabled")).toBeChecked();

    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, {
      timeout: 15000,
    });

    await expect(page.locator("#mouseCaptureEnabled")).toBeChecked();
    await page.evaluate(() => localStorage.removeItem("retro8080.pc486.ui"));
  });

  test("checking/unchecking it doesn't affect the running machine", async ({
    livePage: page,
  }) => {
    const cycles1 = await page.evaluate(
      () => (window as any).__test.machine.totalCycles()
    );

    await page.locator("#mouseCaptureEnabled").check();
    await page.waitForTimeout(200);

    const cycles2 = await page.evaluate(
      () => (window as any).__test.machine.totalCycles()
    );
    expect(cycles2).toBeGreaterThan(cycles1);
  });

  test("injectMouseEvent is callable via the embind API regardless of the UI capture state", async ({
    livePage: page,
  }) => {

    // Relative move plus left-button down/up. No mouse driver runs at a bare FreeDOS prompt, so this
    // only checks inject_mouse_event -> i8042 AUX is reachable and doesn't throw.
    await page.evaluate(() => {
      const m = (window as any).__test.machine;
      m.injectMouseEvent(5, -3, 0);
      m.injectMouseEvent(0, 0, 0x01);
      m.injectMouseEvent(0, 0, 0x00);
    });
  });
});
