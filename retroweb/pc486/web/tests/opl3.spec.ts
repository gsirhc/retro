import { test, expect } from "./fixtures";
import { bootLive } from "./helpers";

// The OPL3 behind the card's FM ports (see opl3.h). These drive the real
// front end in a browser: the point is that a program running on the guest
// can find the chip through the port block and get audio out of it, which is
// what the machine could not do before the OPL3 existed.
//
// Writing FM registers from the page rather than from DOS is deliberate --
// exercising the guest side would need a music driver and a song loaded, and
// these tests are about the hardware surface, the same scope soundblaster.spec.ts
// keeps to.
test.describe("OPL3 FM synthesizer", () => {
  test("the canonical AdLib detection sequence succeeds through base+8h/9h", async ({
    page,
  }) => {
    await bootLive(page);

    const result = await page.evaluate(() => {
      const m = (window as any).__test.machine;
      // Address to base+8h, data to base+9h -- the AdLib-compatible pair.
      const fmWrite = (reg: number, val: number) => { m.portOut(0x228, reg); m.portOut(0x229, val); };
      fmWrite(0x04, 0x60);  // mask and reset both timers
      fmWrite(0x04, 0x80);  // reset the IRQ flags
      const quiet = m.portIn(0x228);
      fmWrite(0x02, 0xff);  // timer 1 preset: expires after one 80.8us tick
      fmWrite(0x04, 0x21);  // mask timer 2, start timer 1
      m.runCycles(20000);      // ~300us at 66MHz, well past one tick
      const fired = m.portIn(0x228);
      fmWrite(0x04, 0x60);
      fmWrite(0x04, 0x80);
      return { quiet, fired, cleared: m.portIn(0x228) };
    });

    expect(result.quiet).toBe(0x00);
    expect(result.fired).toBe(0xc0);
    expect(result.cleared).toBe(0x00);
  });

  test("the detection sequence also succeeds through the AdLib 0x388/0x389 pair", async ({
    page,
  }) => {
    await bootLive(page);

    // This is the pair an AdLib-era music driver actually uses -- it never
    // touches the card's own block, so decoding only base+0h..3h would let a
    // game detect an OPL and then play silence.
    const result = await page.evaluate(() => {
      const m = (window as any).__test.machine;
      const fmWrite = (reg: number, val: number) => { m.portOut(0x388, reg); m.portOut(0x389, val); };
      fmWrite(0x04, 0x60);
      fmWrite(0x04, 0x80);
      const quiet = m.portIn(0x388);
      fmWrite(0x02, 0xff);
      fmWrite(0x04, 0x21);
      m.runCycles(20000);
      const fired = m.portIn(0x388);
      // Same chip as the card's own block, not a second one.
      fmWrite(0x04, 0x80);
      fmWrite(0x20, 0x0a);
      return { quiet, fired, shared: m.fmReg(0x20) };
    });

    expect(result.quiet).toBe(0x00);
    expect(result.fired).toBe(0xc0);
    expect(result.shared).toBe(0x0a);
  });

  test("a keyed-on channel produces non-zero stereo samples at the real 49716 Hz rate", async ({
    page,
  }) => {
    await bootLive(page);

    const result = await page.evaluate(() => {
      const m = (window as any).__test.machine;
      const fmWrite = (reg: number, val: number) => { m.portOut(0x228, reg); m.portOut(0x229, val); };
      fmWrite(0x20, 0x01); fmWrite(0x23, 0x01);  // MULT=1 on both operators
      fmWrite(0x40, 0x10); fmWrite(0x43, 0x00);  // modulator attenuated, carrier loud
      fmWrite(0x60, 0xf0); fmWrite(0x63, 0xf0);  // fast attack
      fmWrite(0x80, 0x00); fmWrite(0x83, 0x00);
      fmWrite(0xc0, 0x30);                          // both outputs enabled
      fmWrite(0xa0, 0x98); fmWrite(0xb0, 0x2e);  // key on, mid octave
      m.fmDrainSamples();                              // discard anything already queued

      const cycles = 660000;  // 10ms at 66MHz
      m.runCycles(cycles);
      const s = m.fmDrainSamples();
      let nonZero = 0;
      for (let i = 0; i < s.left.length; i++) {
        if (s.left[i] !== 0 || s.right[i] !== 0) nonZero++;
      }
      return { count: s.left.length, nonZero, secondDrain: m.fmDrainSamples().left.length };
    });

    // 10ms at 49715.9 Hz is ~497 frames; allow slack for burst granularity.
    expect(result.count).toBeGreaterThan(400);
    expect(result.count).toBeLessThan(600);
    expect(result.nonZero).toBeGreaterThan(0);
    expect(result.secondDrain).toBe(0);
  });

  test("FM audio and digitized audio are separate streams behind their own mixer attenuators", async ({
    page,
  }) => {
    await bootLive(page);

    const result = await page.evaluate(() => {
      const m = (window as any).__test.machine;
      const fm = m.fmDrainSamples();
      return {
        hasCycles: fm.cycles instanceof Float64Array,
        hasLeft: fm.left instanceof Int16Array,
        hasRight: fm.right instanceof Int16Array,
        // The FM leg reads mixer 34h/35h and the voice leg 32h/33h, both
        // through Master -- four independent gains, not one shared number.
        fmL: m.fmGainLeft(), fmR: m.fmGainRight(),
        sbL: m.sbGainLeft(), sbR: m.sbGainRight(),
      };
    });

    expect(result.hasCycles).toBe(true);
    expect(result.hasLeft).toBe(true);
    expect(result.hasRight).toBe(true);
    for (const g of [result.fmL, result.fmR, result.sbL, result.sbR]) {
      expect(g).toBeGreaterThan(0);
      expect(g).toBeLessThanOrEqual(1);
    }
  });

  test("bank 1 at base+2h/3h stays inert until the OPL3 NEW bit is set", async ({
    page,
  }) => {
    await bootLive(page);

    const result = await page.evaluate(() => {
      const m = (window as any).__test.machine;
      m.portOut(0x222, 0x20);  // bank 1, register 20h
      m.portOut(0x223, 0x01);
      const beforeNew = m.fmReg(0x120);
      m.portOut(0x222, 0x05);  // 105h: NEW
      m.portOut(0x223, 0x01);
      const mode = m.fmOpl3Mode();
      m.portOut(0x222, 0x20);
      m.portOut(0x223, 0x01);
      return { beforeNew, mode, afterNew: m.fmReg(0x120) };
    });

    expect(result.beforeNew).toBe(0x00);
    expect(result.mode).toBe(true);
    expect(result.afterNew).toBe(0x01);
  });
});
