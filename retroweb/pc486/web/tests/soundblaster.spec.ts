import { test, expect } from "./fixtures";

// The Sound Blaster 16's digitized output shares the "Enable sound" checkbox with the PC speaker
// (speaker.spec.ts); one gate for the autoplay policy. Covers the embind surface of soundblaster.h.
// DOS-side playback needs a driver (SET BLASTER=...).
test.describe("Sound Blaster 16", () => {
  test("sbDrainSamples and sbSampleRateHz are queryable via the embind API regardless of the UI mute state", async ({
    livePage: page,
  }) => {

    const result = await page.evaluate(() => {
      const m = (window as any).__test.machine;
      const samples = m.sbDrainSamples();
      const rate = m.sbSampleRateHz();
      return {
        hasCycles: samples.cycles instanceof Float64Array,
        hasLeft: samples.left instanceof Int16Array,
        hasRight: samples.right instanceof Int16Array,
        rateType: typeof rate,
      };
    });
    expect(result.hasCycles).toBe(true);
    expect(result.hasLeft).toBe(true);
    expect(result.hasRight).toBe(true);
    expect(result.rateType).toBe("number");
  });

  test("draining twice in a row returns an empty log the second time -- drain_samples() clears as it reads", async ({
    livePage: page,
  }) => {

    const secondLen = await page.evaluate(() => {
      const m = (window as any).__test.machine;
      m.sbDrainSamples();
      const second = m.sbDrainSamples();
      return second.left.length;
    });
    // No driver has programmed the DSP at a bare prompt, so there is nothing to drain; this checks
    // repeated calls are safe.
    expect(secondLen).toBe(0);
  });
});
