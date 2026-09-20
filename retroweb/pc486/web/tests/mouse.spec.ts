import { test, expect } from "./fixtures";
import { boot, waitForScreen } from "./helpers";

// Mouse capture checkbox is unchecked by default on every page load, same
// off-by-default/never-restored policy as the speaker checkbox (see
// speaker.spec.ts) -- Pointer Lock is itself a permission gate, and
// capturing the pointer without an explicit opt-in would trap the user's
// cursor on a page they didn't ask for that on.
test.describe("PS/2 mouse", () => {
  test("capture checkbox is unchecked by default", async ({ page }) => {
    await boot(page);
    await expect(page.locator("#mouseCaptureEnabled")).not.toBeChecked();
  });

  test("stays unchecked across a reload -- never restored from a saved preference", async ({
    page,
  }) => {
    await boot(page);
    await page.locator("#mouseCaptureEnabled").check();
    await expect(page.locator("#mouseCaptureEnabled")).toBeChecked();

    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, {
      timeout: 15000,
    });

    await expect(page.locator("#mouseCaptureEnabled")).not.toBeChecked();
  });

  test("checking/unchecking it doesn't affect the running machine", async ({
    page,
  }) => {
    await boot(page);
    const cycles1 = await page.evaluate(
      () => (window as any).__test.machine.totalCycles()
    );

    await page.locator("#mouseCaptureEnabled").check();
    await page.waitForTimeout(200);

    const cycles2 = await page.evaluate(
      () => (window as any).__test.machine.totalCycles()
    );
    expect(cycles2).toBeGreaterThan(cycles1);
    await waitForScreen(page, /C:\\>/);
  });

  test("injectMouseEvent is callable via the embind API regardless of the UI capture state", async ({
    page,
  }) => {
    await boot(page);

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
