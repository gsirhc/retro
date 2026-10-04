import { test, expect } from "./fixtures";
import { bootLive } from "./helpers";

// `?audiotrace` records one row per audio post so a real game session's
// pacing can be replayed in the harness -- a synthetic main-thread load does
// not reproduce what a DOS game's redraw does to the audio path, which is
// why PC486_REVIEW.md section 31's open defect has outlasted four attempts.
// Opt-in and absent otherwise, like `?fmtrace`.
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

    // Capture with the card actually playing, which is the case this exists
    // for -- and the only one where the ring figure means anything, since it
    // is the audio thread's lead over the card's own samples.
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
    // Every row has to carry a real post length and a real guest-cycle span,
    // because those two are the pair the open defect lives between.
    expect(summary.postMs.p50).toBeGreaterThan(0);
    expect(summary.guestPerWall.p50).toBeGreaterThan(0);
    // The ring figure is the audio thread's lead over the card's samples. It
    // is present here, but this page boots under the fast-test multiplier,
    // where the guest outruns wall time and the audio thread is pinned to
    // whatever has actually been produced -- so the lead legitimately reads
    // zero. tone.spec.ts asserts a real cushion at real speed instead.
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
