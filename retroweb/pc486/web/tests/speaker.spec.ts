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

  // A created AudioContext can still be "suspended" rather than actually
  // producing sound -- some browsers don't auto-resume it on the very
  // gesture that constructed it, and any browser may suspend an idle one
  // later on its own. Either way this fails silently (no error, no audio),
  // which is why ensureAudioStarted() explicitly resumes a suspended
  // context both on creation and every time the box is re-checked.
  test("checking the box leaves the audio context actually running, not merely created", async ({
    page,
  }) => {
    await boot(page);
    await page.locator("#speakerEnabled").check();
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.audioState))
      .toBe("running");
  });

  // AudioWorklet doesn't exist at all outside a "secure context" (https://,
  // or http://localhost specifically -- a LAN IP/hostname on your own
  // network does not count, even though nothing about that setup is
  // actually insecure). The deployed site is always https:// so a real
  // visitor never hits this, but a locally-served LAN preview can -- and
  // did, live: audioCtx.audioWorklet was undefined, and calling
  // .addModule() on it threw an uncaught TypeError from the checkbox's own
  // change handler. Simulates that exact shape by constructing a real
  // AudioContext and then hiding its audioWorklet, the same as Chrome
  // itself does outside a secure context.
  test("missing AudioWorklet (e.g. an insecure-context LAN preview) degrades to silent instead of throwing", async ({
    page,
  }) => {
    await page.addInitScript(() => {
      const OrigAC = window.AudioContext || (window as any).webkitAudioContext;
      class NoWorkletAudioContext extends OrigAC {
        constructor(...args: any[]) {
          super(...args);
          Object.defineProperty(this, "audioWorklet", { value: undefined, configurable: true });
        }
      }
      (window as any).AudioContext = NoWorkletAudioContext;
    });
    const pageErrors: Error[] = [];
    page.on("pageerror", (e) => pageErrors.push(e));

    await boot(page);
    await page.locator("#speakerEnabled").check();
    // Give ensureAudioStarted's async work a moment to run (and, if the
    // guard were missing, to throw) before asserting nothing did.
    await page.waitForTimeout(300);

    expect(pageErrors).toEqual([]);
    // machine keeps running regardless -- audio being unavailable never
    // affects emulation itself
    const cycles1 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    await page.waitForTimeout(200);
    const cycles2 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    expect(cycles2).toBeGreaterThan(cycles1);
  });
});
