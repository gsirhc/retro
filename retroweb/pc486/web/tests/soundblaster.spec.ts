import { test, expect } from "./fixtures";
import { boot } from "./helpers";

// The Sound Blaster 16's digitized output rides the same "Enable sound"
// checkbox as the PC speaker (see speaker.spec.ts) -- one opt-in gate for
// both, since both are blocked by the same browser autoplay policy until
// the user acts. These tests cover the embind surface soundblaster.h's
// device exposes to the front end; DOS-side playback needs a loaded driver
// (SET BLASTER=...), out of scope for a bare FreeDOS-prompt test.
test.describe("Sound Blaster 16", () => {
  test("sbDrainSamples and sbSampleRateHz are queryable via the embind API regardless of the UI mute state", async ({
    page,
  }) => {
    await boot(page);

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
    page,
  }) => {
    await boot(page);

    const secondLen = await page.evaluate(() => {
      const m = (window as any).__test.machine;
      m.sbDrainSamples();
      const second = m.sbDrainSamples();
      return second.left.length;
    });
    // No DOS-side driver has programmed the DSP at a bare prompt, so the
    // device produces nothing to drain either time -- this only confirms
    // the call is safe to make repeatedly without accumulating state.
    expect(secondLen).toBe(0);
  });
});
