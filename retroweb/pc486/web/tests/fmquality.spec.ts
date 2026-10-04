import { test, expect } from "./fixtures";
import { bootLive } from "./helpers";

// What the card actually puts on the wire, measured rather than assumed.
// A pure OPL3 sine should stay a pure sine through the front end's
// resampling: the emulator produces audio on the guest's clock and the
// device consumes it on its own, and the buffer handed over has to be sized
// in the same timebase it is filled from. When it was sized from wall-clock
// time instead, every post ended in a held fragment -- a buzz at the post
// rate, worst while the guest was busy. This runs at REAL speed on purpose
// (no fast=1): the fast-test multiplier decimates the FM stream 20:1 and
// measures a completely different resampling regime.
type Capture = { samples: number[]; cyc: number[] };

// Rising zero crossings, as fractional sample positions.
function crossings(samples: number[]) {
  const zc: number[] = [];
  for (let i = 1; i < samples.length; i++) {
    if (samples[i - 1] < 0 && samples[i] >= 0) {
      zc.push(i - 1 + -samples[i - 1] / (samples[i] - samples[i - 1]));
    }
  }
  return zc;
}

// Period jitter as heard, in output samples.
function zeroCrossingJitter(samples: number[]) {
  const zc = crossings(samples);
  const iv: number[] = [];
  for (let i = 1; i < zc.length; i++) iv.push(zc[i] - zc[i - 1]);
  const mean = iv.reduce((a, b) => a + b, 0) / iv.length;
  const sd = Math.sqrt(iv.reduce((a, b) => a + (b - mean) ** 2, 0) / iv.length);
  const median = [...iv].sort((a, b) => a - b)[iv.length >> 1];
  return { count: iv.length, mean, median, cv: sd / mean };
}

// The same periods measured in guest cycles, from the cycle the worklet
// says each output sample played. Whatever pace the worklet chose, every
// period of the tone should span the same number of guest cycles.
function guestPeriods(c: Capture) {
  const at = (x: number) => {
    const i = Math.floor(x);
    return c.cyc[i] + (x - i) * (c.cyc[i + 1] - c.cyc[i]);
  };
  const zc = crossings(c.samples).filter((x) => x + 1 < c.cyc.length);
  const periods: number[] = [];
  for (let i = 1; i < zc.length; i++) periods.push(at(zc[i]) - at(zc[i - 1]));
  const median = [...periods].sort((a, b) => a - b)[periods.length >> 1];
  const off = periods.filter((p) => Math.abs(p - median) / median > 0.05).length / periods.length;
  return { median, off };
}

test.describe("FM output quality", () => {
  test("a pure OPL3 tone comes through clean, idle and under main-thread load", async ({ page }) => {
    test.setTimeout(120000);
    await bootLive(page, { realtime: true });
    await page.locator("#speakerEnabled").check();
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.audioState))
      .toBe("running");

    // Carrier only: additive connection with the modulator fully attenuated,
    // so nothing modulates the phase and the output is one sine. fnum 0x198
    // at block 4, MULT 1 => 408 * 49715.9 / 2^20 * 2^4 = 309.5 Hz.
    await page.evaluate(() => {
      const m = (window as any).__test.machine;
      const w = (r: number, v: number) => { m.portOut(0x388, r); m.portOut(0x389, v); };
      w(0x01, 0x20); w(0xBD, 0x00);
      w(0x20, 0x21); w(0x23, 0x21);
      w(0x40, 0x3F); w(0x43, 0x00);
      w(0x60, 0xFF); w(0x63, 0xF0);
      w(0x80, 0x00); w(0x83, 0x00);
      w(0xC0, 0x31);
      w(0xA0, 0x98); w(0xB0, 0x31);
    });
    await page.waitForTimeout(1500);

    const capture = async (load: boolean) => {
      await page.evaluate(() => (window as any).__test.startSbCapture(48000));
      if (load) {
        // Occupy the main thread the way a heavy guest redraw does, in
        // bursts the emulator can still keep its clock through -- a load
        // heavy enough to push the guest below real time genuinely starves
        // the audio, because the machine has not produced it yet.
        for (let i = 0; i < 50; i++) {
          await page.evaluate(() => { const e = performance.now() + 8; while (performance.now() < e); });
          await page.waitForTimeout(12);
        }
      } else {
        await page.waitForTimeout(1000);
      }
      // The capture comes back from the audio thread once it has the
      // requested number of samples -- it is what actually reached the
      // device, not what the main thread hoped would.
      await expect
        .poll(() => page.evaluate(() => (window as any).__test.hasSbCapture), { timeout: 20000 })
        .toBe(true);
      return (await page.evaluate(() => {
        const t = (window as any).__test;
        return { samples: t.takeSbCapture() as number[], cyc: t.sbCaptureCyc as number[] };
      })) as Capture;
    };

    const sampleRate: number = await page.evaluate(() => (window as any).__test.audioSampleRate);
    const idleCapture = await capture(false);
    const loadedCapture = await capture(true);

    // Playback speed the worklet chose: guest time per period over heard
    // time per period. 1.0 when the host kept real time.
    const speed = (c: Capture) =>
      (guestPeriods(c).median / 66_000_000) / (zeroCrossingJitter(c.samples).median / sampleRate);
    // The host kept up: real speed and no period spanning a starved hold.
    const keptUp = (c: Capture) => speed(c) >= 0.99 && guestPeriods(c).off === 0;

    for (const [name, c] of [["idle", idleCapture], ["loaded", loadedCapture]] as const) {
      const g = guestPeriods(c);
      test.info().annotations.push({ type: name, description: `speed ${speed(c).toFixed(3)}, off ${(g.off * 100).toFixed(1)}%` });
      // In guest time the tone is 309.5 Hz exactly: 66 MHz / 309.5 = 213,247
      // cycles a period. Samples landing where their stamps say keeps every
      // period there. When the main thread delivers audio late the worklet
      // slows playback a little, or holds, on purpose rather than drop out
      // -- that changes what a listener hears, not the guest-time period,
      // except for the odd period spanning a hold.
      expect(66_000_000 / g.median).toBeGreaterThan(305);
      expect(66_000_000 / g.median).toBeLessThan(315);
      expect(g.off).toBeLessThan(0.05);
    }

    // As heard, on a host that keeps real time: 309.5 Hz, and steady.
    // Placing samples by this chunk's measured cycles-per-second instead of
    // the CPU's own clock bent this to 286 Hz with 18% period jitter as soon
    // as the main thread was busy, because a chunk whose wall window held a
    // stall ran fewer cycles than that window and its audio got spread
    // across all of it. A host that fell behind gets the guest-time checks
    // above only, since the worklet's deliberate slowdown and holds are
    // what it hears.
    const idle = zeroCrossingJitter(idleCapture.samples);
    const loaded = zeroCrossingJitter(loadedCapture.samples);
    if (keptUp(idleCapture)) {
      expect(sampleRate / idle.mean).toBeGreaterThan(305);
      expect(sampleRate / idle.mean).toBeLessThan(315);
      expect(idle.cv).toBeLessThan(0.02);
    }
    // And under main-thread load, which is where this used to fall apart:
    // placing samples on the main thread from performance.now() deltas gave
    // ~286 Hz with ~18% jitter here, because runCycles() blocks that thread
    // for up to 12ms and the ratio was derived over ~1ms windows. The audio
    // thread's own clock has no such problem.
    if (keptUp(loadedCapture)) {
      expect(sampleRate / loaded.mean).toBeGreaterThan(303);
      expect(sampleRate / loaded.mean).toBeLessThan(317);
      expect(loaded.cv).toBeLessThan(0.03);
    }
  });
});
