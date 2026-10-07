import { test, expect } from "./fixtures";
import { bootLive } from "./helpers";

// `?audiotrace` records one row per audio post so a real game session's pacing can be replayed.
// Opt-in, like `?fmtrace`. See PC486_REVIEW.md section 31.
test.describe("audio trace", () => {
  test("is absent without the parameter", async ({ livePage: page }) => {
    expect(await page.evaluate(() => (window as any).__audio === undefined)).toBe(true);
  });

  test("records per-post pacing once started, and summarises it", async ({ page }) => {
    test.setTimeout(120000);
    await bootLive(page, { params: "audiotrace" });
    await page.locator("#speakerEnabled").check();
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.audioState))
      .toBe("running");

    // Capture with the card playing; the ring figure is the audio thread's lead over the card's samples.
    await page.evaluate(() => {
      const m = (window as any).__test.machine;
      const w = (r: number, v: number) => { m.portOut(0x388, r); m.portOut(0x389, v); };
      w(0x01, 0x20); w(0x20, 0x21); w(0x23, 0x21); w(0x40, 0x3F); w(0x43, 0x00);
      w(0x60, 0xFF); w(0x63, 0xF0); w(0x80, 0x00); w(0x83, 0x00);
      w(0xC0, 0x31); w(0xA0, 0x98); w(0xB0, 0x31);
    });
    await page.waitForTimeout(1000);
    expect(await page.evaluate(() => (window as any).__audio.start(5000))).toBe("recording");
    await page.waitForTimeout(1500);
    const rows = await page.evaluate(() => (window as any).__audio.stop());
    expect(rows).toBeGreaterThan(10);

    const summary = await page.evaluate(() => (window as any).__audio.summary());
    expect(summary.rows).toBe(rows);
    expect(summary.postMs.p50).toBeGreaterThan(0);
    expect(summary.guestPerWall.p50).toBeGreaterThan(0);
    // The ring lead reads zero under the fast-test multiplier. tone.spec.ts asserts a real cushion at real speed.
    expect(summary.ringMs).not.toBeNull();
    expect(summary.ringMs.p50).toBeGreaterThanOrEqual(0);
  });

  test("records nothing until started", async ({ page }) => {
    await bootLive(page, { params: "audiotrace" });
    await page.locator("#speakerEnabled").check();
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.audioState))
      .toBe("running");
    await page.waitForTimeout(500);
    expect(await page.evaluate(() => (window as any).__audio.summary()))
      .toBe("no rows -- is sound enabled?");
  });
});
