import { test, expect } from "./fixtures";
import { bootLive } from "./helpers";

// The CT1745 mixer's tone controls (Creative's Sound Blaster Series Hardware
// Programming Guide, chapter 4): registers 44h/45h (Treble L/R) and 46h/47h
// (Bass L/R), 4 bits in the value's high nibble, default 8<<4 -- 0 to 7 is
// -14 dB to 0 dB and 8 to 15 is 0 dB to +14 dB, both in 2 dB steps. These sit
// in the mixer, upstream of the card's own output amp, so app.js models them
// as shelving filters ahead of the backplate-wheel gain node rather than as
// another gain multiplier on the sample stream.
test.describe("Sound Blaster 16 tone controls", () => {
  async function startAudio(page: any) {
    await page.locator("#speakerEnabled").check();
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.audioState))
      .toBe("running");
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.sbToneGains()))
      .not.toBeNull();
  }

  function mixerWrite(page: any, reg: number, value: number) {
    return page.evaluate(({ reg, value }: { reg: number; value: number }) => {
      const m = (window as any).__test.machine;
      m.portOut(0x224, reg);
      m.portOut(0x225, value);
    }, { reg, value });
  }

  test("default registers give flat (0 dB) filters on both channels", async ({
    livePage: page,
  }) => {
    await startAudio(page);
    const gains = await page.evaluate(() => (window as any).__test.sbToneGains());
    expect(gains.trebleLeft).toBe(0);
    expect(gains.trebleRight).toBe(0);
    expect(gains.bassLeft).toBe(0);
    expect(gains.bassRight).toBe(0);
  });

  test("level 0 gives -14 dB", async ({ livePage: page }) => {
    await startAudio(page);
    await mixerWrite(page, 0x44, 0x00);  // Treble L
    await mixerWrite(page, 0x46, 0x00);  // Bass L
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.sbToneGains().trebleLeft))
      .toBe(-14);
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.sbToneGains().bassLeft))
      .toBe(-14);
  });

  test("level 15 gives +14 dB", async ({ livePage: page }) => {
    await startAudio(page);
    await mixerWrite(page, 0x45, 0xf0);  // Treble R
    await mixerWrite(page, 0x47, 0xf0);  // Bass R
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.sbToneGains().trebleRight))
      .toBe(14);
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.sbToneGains().bassRight))
      .toBe(14);
  });

  test("levels 7 and 8 both give 0 dB", async ({ livePage: page }) => {
    await startAudio(page);
    await mixerWrite(page, 0x44, 0x70);  // level 7
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.sbToneGains().trebleLeft))
      .toBe(0);
    await mixerWrite(page, 0x44, 0x80);  // level 8
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.sbToneGains().trebleLeft))
      .toBe(0);
  });

  test("left and right channels are independent", async ({ livePage: page }) => {
    await startAudio(page);
    await mixerWrite(page, 0x44, 0x00);  // Treble L -> -14 dB
    await mixerWrite(page, 0x45, 0xf0);  // Treble R -> +14 dB
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.sbToneGains().trebleLeft))
      .toBe(-14);
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.sbToneGains().trebleRight))
      .toBe(14);
  });
});

// The ring buffer's own depth. The worklet's 50ms target is a ceiling it
// trims down to, never a floor anything builds up to, so the depth that
// actually holds is whatever the pump's posting cadence leaves standing --
// and that cushion is what absorbs a main-thread hitch before the worklet
// runs dry and holds its last sample. Idle, it sits in the thirties.
test.describe("Sound Blaster 16 audio ring", () => {
  // At real speed, deliberately: the lead the audio thread holds is a
  // real-time property, and the fast-test multiplier runs the guest faster
  // than wall time on purpose, so there the audio thread is pinned to
  // whatever the machine has produced and the figure means nothing.
  test("holds a working cushion while audio is flowing", async ({ page }) => {
    test.setTimeout(60000);
    await bootLive(page, { realtime: true });
    await page.locator("#speakerEnabled").check();
    // Depth is the lead the audio thread holds over the card's own samples,
    // so it only exists while the card is producing any: with the machine
    // silent there is nothing to be ahead of. Key a note on first.
    await page.evaluate(() => {
      const m = (window as any).__test.machine;
      const w = (r: number, v: number) => { m.portOut(0x388, r); m.portOut(0x389, v); };
      w(0x01, 0x20); w(0x20, 0x21); w(0x23, 0x21); w(0x40, 0x3F); w(0x43, 0x00);
      w(0x60, 0xFF); w(0x63, 0xF0); w(0x80, 0x00); w(0x83, 0x00);
      w(0xC0, 0x31); w(0xA0, 0x98); w(0xB0, 0x31);
    });
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.audioState))
      .toBe("running");
    // The worklet reports twice a second, so the first figure takes a moment.
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.sbRingMs), { timeout: 15000 })
      .not.toBeNull();
    // The worklet reports twice a second and zeroes its counters each time,
    // so the first report covers the ring filling from empty and legitimately
    // shows starvation. What matters is that it settles and stays settled.
    await page.waitForTimeout(1500);
    const ringMs = await page.evaluate(() => (window as any).__test.sbRingMs);
    expect(ringMs).toBeGreaterThan(50);
    expect(ringMs).toBeLessThanOrEqual(130);
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.audioStatsRaw.starved), { timeout: 10000 })
      .toBe(0);
  });
});
