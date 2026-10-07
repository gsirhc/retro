import { test, expect } from "./fixtures";
import { bootLive } from "./helpers";

// The speaker checkbox is muted by default. Enable Sound is remembered in retro8080.pc486.ui;
// browsers still need a gesture. The C++ device tracks speaker state independent of the checkbox.
test.describe("PC speaker", () => {
  test("is unchecked (muted) by default", async ({ livePage: page }) => {
    await expect(page.locator("#speakerEnabled")).not.toBeChecked();
  });

  test("Enable Sound preference persists across reload", async ({ page }) => {
    await bootLive(page);
    await page.evaluate(() => localStorage.removeItem("retro8080.pc486.ui"));
    await page.locator("#speakerEnabled").check();
    await expect(page.locator("#speakerEnabled")).toBeChecked();

    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, {
      timeout: 15000,
    });

    await expect(page.locator("#speakerEnabled")).toBeChecked();
    await page.evaluate(() => localStorage.removeItem("retro8080.pc486.ui"));
  });

  // A checkbox restored from localStorage never fires "change", so nothing created the AudioContext.
  // Clicking the screen, the first gesture every visitor makes, has to be enough.
  test("a restored Enable Sound checkmark actually starts audio on the first screen click, not just the checkbox", async ({ page }) => {
    await bootLive(page);
    await page.evaluate(() => localStorage.removeItem("retro8080.pc486.ui"));
    await page.locator("#speakerEnabled").check();
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, {
      timeout: 15000,
    });
    await expect(page.locator("#speakerEnabled")).toBeChecked();
    expect(await page.evaluate(() => (window as any).__test.audioState)).toBeFalsy();

    await page.locator("#screen").click();
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.audioState))
      .toBe("running");
    await page.evaluate(() => localStorage.removeItem("retro8080.pc486.ui"));
  });

  test("checking/unchecking it doesn't affect the running machine", async ({
    livePage: page,
  }) => {
    const cycles1 = await page.evaluate(
      () => (window as any).__test.machine.totalCycles()
    );

    await page.locator("#speakerEnabled").check();
    await page.waitForTimeout(200);

    const cycles2 = await page.evaluate(
      () => (window as any).__test.machine.totalCycles()
    );
    expect(cycles2).toBeGreaterThan(cycles1);
  });

  test("the underlying speaker device is queryable via the embind API regardless of the UI mute state", async ({
    livePage: page,
  }) => {

    const level = await page.evaluate(
      () => (window as any).__test.machine.speakerLevel()
    );
    expect(typeof level).toBe("boolean");
  });

  // A created AudioContext can still be suspended, silently. ensureAudioStarted() resumes it on
  // creation and on every re-check.
  test("checking the box leaves the audio context actually running, not merely created", async ({
    livePage: page,
  }) => {
    await page.locator("#speakerEnabled").check();
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.audioState))
      .toBe("running");
  });

  // Unchecking must suspend the context, or the tab's speaker icon stays lit.
  test("unchecking the box suspends the audio context", async ({ livePage: page }) => {
    await page.locator("#speakerEnabled").check();
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.audioState))
      .toBe("running");
    await page.locator("#speakerEnabled").uncheck();
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.audioState))
      .toBe("suspended");
  });

  // AudioWorklet doesn't exist outside a secure context (https, or localhost but not a LAN IP).
  // A LAN preview hit this: addModule() threw from the checkbox's change handler. Simulated by
  // hiding audioWorklet on a real AudioContext, as Chrome does.
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

    await bootLive(page);
    await page.locator("#speakerEnabled").check();
    // Give ensureAudioStarted's async work a moment to throw if the guard were missing.
    await page.waitForTimeout(300);

    expect(pageErrors).toEqual([]);
    // Emulation keeps running regardless.
    const cycles1 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    await page.waitForTimeout(200);
    const cycles2 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    expect(cycles2).toBeGreaterThan(cycles1);
  });
});
