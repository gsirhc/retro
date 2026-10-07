import { test, expect } from "./fixtures";
import { bootLive } from "./helpers";

// CT1745 mixer tone controls (Creative's SB Series Hardware Programming Guide, ch. 4): 44h/45h
// Treble L/R, 46h/47h Bass L/R, 4 bits in the high nibble, default 8<<4. 0-7 is -14 to 0 dB,
// 8-15 is 0 to +14 dB, 2 dB steps. They sit upstream of the output amp, so app.js models them as
// shelving filters ahead of the wheel gain node.
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

// Ring depth. The worklet's 50ms target is a ceiling it trims to, so the depth that holds is what
// the pump's cadence leaves; that cushion absorbs a main-thread hitch. Idle, it sits in the thirties.
test.describe("Sound Blaster 16 audio ring", () => {
  // Real speed on purpose: the audio lead is a real-time property, and under the fast multiplier the
  // audio thread is pinned to what the machine produced.
  test("holds a working cushion while audio is flowing", async ({ page }) => {
    test.setTimeout(60000);
    await bootLive(page, { realtime: true });
    await page.locator("#speakerEnabled").check();
    // Depth is the audio thread's lead over the card's samples, so key a note on first.
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
    // The worklet reports twice a second.
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.sbRingMs), { timeout: 15000 })
      .not.toBeNull();
    // It zeroes its counters each report, so the first covers the ring filling and shows starvation.
    // What matters is that it settles and stays settled.
    await page.waitForTimeout(1500);
    const ringMs = await page.evaluate(() => (window as any).__test.sbRingMs);
    expect(ringMs).toBeGreaterThan(50);
    expect(ringMs).toBeLessThanOrEqual(130);
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.audioStatsRaw.starved), { timeout: 10000 })
      .toBe(0);
  });
});
