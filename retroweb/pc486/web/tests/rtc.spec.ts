import { test, expect } from "./fixtures";
import { bootLive } from "./helpers";

// Battery-backed RTC: power-on reads the visitor's local clock, then keeps it in guest time.
async function readRtc(page: any) {
  return page.evaluate(() => {
    const m = (window as any).__test.machine;
    const reg = (r: number) => { m.portOut(0x70, r); return m.portIn(0x71); };
    const bcd = (v: number) => (v >> 4) * 10 + (v & 0x0f);
    return {
      sec: bcd(reg(0x00)), min: bcd(reg(0x02)), hour: bcd(reg(0x04)),
      day: bcd(reg(0x07)), month: bcd(reg(0x08)), year: bcd(reg(0x32)) * 100 + bcd(reg(0x09)),
    };
  });
}

test.describe("real-time clock", () => {
  test("powers on at the visitor's local time and keeps running", async ({ page }) => {
    await bootLive(page, { realtime: true });
    const now = new Date();
    const rtc = await readRtc(page);
    expect(rtc.year).toBe(now.getFullYear());
    expect(rtc.month).toBe(now.getMonth() + 1);
    expect(rtc.day).toBe(now.getDate());
    const rtcMinutes = rtc.hour * 60 + rtc.min;
    const hostMinutes = now.getHours() * 60 + now.getMinutes();
    expect(Math.abs(rtcMinutes - hostMinutes) % (24 * 60)).toBeLessThanOrEqual(1);

    test.skip(!!process.env.SLOW_HOST, "host can't hold real 66 MHz; pace checked on the self-hosted runner");
    // Same start-up settle as smoke.spec.ts before measuring the clock's pace.
    await page.waitForTimeout(1000);
    const settled = await readRtc(page);
    const before = settled.min * 60 + settled.sec;
    await page.waitForTimeout(2500);
    const later = await readRtc(page);
    const elapsed = (later.min * 60 + later.sec - before + 3600) % 3600;
    expect(elapsed).toBeGreaterThanOrEqual(2);
    expect(elapsed).toBeLessThanOrEqual(4);
  });
});
