import { test, expect } from "./fixtures";
import { boot, waitForScreen } from "./helpers";

// PC speaker checkbox is muted (unchecked) by default on every page load
// and deliberately never restored from saved preference — browser audio
// requires fresh user gesture anyway, and the default-off policy persists
// across visits. The C++ device tracks its own speaker state independent
// of the front end's mute checkbox.
test.describe("PC speaker", () => {
  test("is unchecked (muted) by default", async ({ page }) => {
    await boot(page);
    await expect(page.locator("#speakerEnabled")).not.toBeChecked();
  });

  test("stays unchecked across a reload -- never restored from a saved preference", async ({
    page,
  }) => {
    await boot(page);
    await page.locator("#speakerEnabled").check();
    await expect(page.locator("#speakerEnabled")).toBeChecked();

    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, {
      timeout: 15000,
    });

    await expect(page.locator("#speakerEnabled")).not.toBeChecked();
  });

  test("checking/unchecking it doesn't affect the running machine", async ({
    page,
  }) => {
    await boot(page);
    const cycles1 = await page.evaluate(
      () => (window as any).__test.machine.totalCycles()
    );

    await page.locator("#speakerEnabled").check();
    await page.waitForTimeout(200);

    const cycles2 = await page.evaluate(
      () => (window as any).__test.machine.totalCycles()
    );
    expect(cycles2).toBeGreaterThan(cycles1);
    await waitForScreen(page, /C:\\>/);
  });

  test("the underlying speaker device is queryable via the embind API regardless of the UI mute state", async ({
    page,
  }) => {
    await boot(page);

    const level = await page.evaluate(
      () => (window as any).__test.machine.speakerLevel()
    );
    expect(typeof level).toBe("boolean");
  });
});
