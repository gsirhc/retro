import { test, expect } from "./fixtures";
import { bootLive } from "./helpers";

// Measures what the card puts on the wire. A pure OPL3 sine must stay pure through resampling.
// The buffer has to be sized in the guest timebase it is filled from; sizing it from wall-clock
// time left a held fragment in every post (a buzz at the post rate). Runs at real speed on
// purpose: fast=1 decimates the FM stream 20:1.
type Capture = { samples: number[]; cyc: number[] };

// Rising zero crossings as fractional sample positions.
function crossings(samples: number[]) {
  const zc: number[] = [];
  for (let i = 1; i < samples.length; i++) {
    if (samples[i - 1] < 0 && samples[i] >= 0) {
      zc.push(i - 1 + -samples[i - 1] / (samples[i] - samples[i - 1]));
    }
  }
  return zc;
}

// Period jitter in output samples.
function zeroCrossingJitter(samples: number[]) {
  const zc = crossings(samples);
  const iv: number[] = [];
  for (let i = 1; i < zc.length; i++) iv.push(zc[i] - zc[i - 1]);
  const mean = iv.reduce((a, b) => a + b, 0) / iv.length;
  const sd = Math.sqrt(iv.reduce((a, b) => a + (b - mean) ** 2, 0) / iv.length);
  const median = [...iv].sort((a, b) => a - b)[iv.length >> 1];
  return { count: iv.length, mean, median, cv: sd / mean };
}

// Periods in guest cycles, from the cycle the worklet says each sample played. Every period
// should span the same count; periods spanning a hold or re-anchor are dropouts and are excluded.
function guestPeriods(c: Capture, sampleRate: number) {
  const nominal = 66_000_000 / sampleRate;
  const broken = c.cyc.map((v, i) => {
    const d = i + 1 < c.cyc.length ? c.cyc[i + 1] - v : nominal;
    return d < 0.9 * nominal || d > 1.1 * nominal;
  });
  const at = (x: number) => {
    const i = Math.floor(x);
    return c.cyc[i] + (x - i) * (c.cyc[i + 1] - c.cyc[i]);
  };
  const zc = crossings(c.samples).filter((x) => x + 1 < c.cyc.length);
  const periods: number[] = [];
  for (let i = 1; i < zc.length; i++) {
    if (broken.slice(Math.floor(zc[i - 1]), Math.ceil(zc[i]) + 1).some(Boolean)) continue;
    periods.push(at(zc[i]) - at(zc[i - 1]));
  }
  const median = [...periods].sort((a, b) => a - b)[periods.length >> 1];
  const off = periods.filter((p) => Math.abs(p - median) / median > 0.05).length / periods.length;
  return { median, off, clean: periods.length, total: Math.max(0, zc.length - 1) };
}

test.describe("FM output quality", () => {
  test("a pure OPL3 tone comes through clean, idle and under main-thread load", async ({ page }) => {
    test.setTimeout(120000);
    await bootLive(page, { realtime: true });
    await page.locator("#speakerEnabled").check();
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.audioState))
      .toBe("running");

    // Carrier only: additive connection, modulator fully attenuated. fnum 0x198, block 4, MULT 1:
    // 408 * 49715.9 / 2^20 * 2^4 = 309.5 Hz.
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
        // Occupy the main thread like a heavy guest redraw, in bursts the emulator can keep its clock
        // through. A load that drops the guest below real time starves the audio.
        for (let i = 0; i < 50; i++) {
          await page.evaluate(() => { const e = performance.now() + 8; while (performance.now() < e); });
          await page.waitForTimeout(12);
        }
      } else {
        await page.waitForTimeout(1000);
      }
      // The capture comes from the audio thread, so it is what reached the device.
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

    // Playback speed the worklet chose: guest time per period over heard time per period.
    const speed = (c: Capture) =>
      (guestPeriods(c, sampleRate).median / 66_000_000) / (zeroCrossingJitter(c.samples).median / sampleRate);
    const keptUp = (c: Capture) => {
      const g = guestPeriods(c, sampleRate);
      return speed(c) >= 0.99 && g.clean === g.total && g.off === 0;
    };

    for (const [name, c] of [["idle", idleCapture], ["loaded", loadedCapture]] as const) {
      const g = guestPeriods(c, sampleRate);
      test.info().annotations.push({
        type: name,
        description: `speed ${speed(c).toFixed(3)}, clean ${g.clean}/${g.total}, off ${(g.off * 100).toFixed(1)}%`,
      });
      // A starved host that rarely plays cleanly says nothing about placement.
      test.skip(g.clean < 50, `${name}: only ${g.clean} clean periods, host too starved to judge`);
      // In guest time the tone is 66 MHz / 309.5 = 213,247 cycles a period. Late audio makes the
      // worklet slow or hold on purpose, which changes the heard period but not the guest one.
      expect(66_000_000 / g.median).toBeGreaterThan(305);
      expect(66_000_000 / g.median).toBeLessThan(315);
      expect(g.off).toBeLessThan(0.05);
    }

    // As heard on a host keeping real time: 309.5 Hz, steady. Placing samples by a chunk's measured
    // cycles-per-second bent this to 286 Hz with 18% jitter under load; a lagging host gets only
    // the guest-time checks.
    const idle = zeroCrossingJitter(idleCapture.samples);
    const loaded = zeroCrossingJitter(loadedCapture.samples);
    if (keptUp(idleCapture)) {
      expect(sampleRate / idle.mean).toBeGreaterThan(305);
      expect(sampleRate / idle.mean).toBeLessThan(315);
      expect(idle.cv).toBeLessThan(0.02);
    }
    // Under main-thread load. Deriving the ratio from performance.now() over ~1ms windows gave
    // ~286 Hz with ~18% jitter because runCycles() blocks for up to 12ms.
    if (keptUp(loadedCapture)) {
      expect(sampleRate / loaded.mean).toBeGreaterThan(303);
      expect(sampleRate / loaded.mean).toBeLessThan(317);
      expect(loaded.cv).toBeLessThan(0.03);
    }
  });
});
