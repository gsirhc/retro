import { test, expect } from "./fixtures";
import { bootLive } from "./helpers";

// Mouse capture is unchecked by default for a first visit. Once chosen,
// Enable Mouse is remembered in retro8080.pc486.ui -- Pointer Lock still
// needs an explicit canvas click to engage.
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

    // A relative move plus a left-button-down/up round trip -- doesn't
    // assert on any DOS-side effect (no mouse driver is loaded at a bare
    // FreeDOS prompt), just that the chipset-level plumbing (chipset.h's
    // inject_mouse_event -> i8042's AUX port) is reachable from JS and
    // doesn't throw.
    await page.evaluate(() => {
      const m = (window as any).__test.machine;
      m.injectMouseEvent(5, -3, 0);
      m.injectMouseEvent(0, 0, 0x01);
      m.injectMouseEvent(0, 0, 0x00);
    });
  });
});
