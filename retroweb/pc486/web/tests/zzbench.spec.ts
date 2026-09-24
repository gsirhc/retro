import { test } from "./fixtures";
import { boot, typeStr, waitForScreen } from "./helpers";

// SCRATCH throughput benchmark -- not part of the suite, deleted before the
// work lands. Boots, launches BOOM, waits for its attract demo to render,
// then times synchronous runCycles() windows in the real browser build.
//
// BENCH_REALTIME=1 boots the genuinely real-speed page (`?test=1`, no
// `fast=1`) so the pump-driven figure is what an actual visitor gets;
// without it the boot uses the fast-test multiplier to reach BOOM sooner,
// which leaves the synchronous runCycles() figure unaffected either way.
// PROFILE=1 adds a CDP sampling profile over BOOM.
test.describe("bench", () => {
  test.setTimeout(900_000);
  test("BOOM gameplay throughput", async ({ page }) => {
    const realtime = !!process.env.BENCH_REALTIME;
    await boot(page, { params: "", realtime, timeout: 600_000 });
    await typeStr(page, "cd \\games\\boom");
    await waitForScreen(page, /BOOM>/, 120_000);
    await typeStr(page, "boom");

    // Wait for mode 13h with genuinely changing frames.
    await page.waitForFunction(
      () => {
        const m = (window as any).__test.machine;
        if (m.renderWidth() !== 320 || m.renderHeight() !== 200) return false;
        const g: any = window as any;
        const rgba = m.renderFrame(true);
        let h = 0;
        for (let i = 0; i < rgba.length; i += 997) h = (h * 31 + rgba[i]) | 0;
        if (g.__lastHash === undefined) { g.__lastHash = h; g.__run = 0; return false; }
        if (h !== g.__lastHash) { g.__run = (g.__run || 0) + 1; g.__lastHash = h; }
        return (g.__run || 0) >= 6;
      },
      null,
      { timeout: 600_000, polling: 250 },
    );

    // Into a live game: any key during the attract demo opens BOOM's menu,
    // then four ENTERs walk episode/skill down into a started level.
    for (let i = 0; i < 5; i++) {
      await page.evaluate(() => (window as any).__test.sendKey("Enter", false));
      await page.waitForTimeout(60);
      await page.evaluate(() => (window as any).__test.sendKey("Enter", true));
      await page.waitForTimeout(3000);
    }
    await page.waitForTimeout(8000);

    const thumb = await page.evaluate(() => {
      const m = (window as any).__test.machine;
      const rgba = m.renderFrame(true), w = m.renderWidth(), h = m.renderHeight();
      let out = "";
      for (let y = 0; y < h; y += 8) {
        for (let x = 0; x < w; x += 4) {
          const i = (y * w + x) * 4;
          const lum = ((rgba[i] * 30 + rgba[i + 1] * 59 + rgba[i + 2] * 11) / 100) | 0;
          out += " .:-=+*#%@"[((lum * 9) / 255) | 0];
        }
        out += "\n";
      }
      return out;
    });
    console.log(thumb);

    // Pump-driven rate: what the page actually achieves end to end. Measured
    // over a long enough window that one shed frame cannot dominate it.
    const paced = await page.evaluate(async () => {
      const m = (window as any).__test.machine;
      const c0 = m.totalCycles(), t0 = performance.now();
      await new Promise((r) => setTimeout(r, 10000));
      return ((m.totalCycles() - c0) / (performance.now() - t0)) * 1000 / 1e6;
    });

    if (process.env.PROFILE) {
      const cdp = await page.context().newCDPSession(page);
      await cdp.send("Profiler.enable");
      await cdp.send("Profiler.setSamplingInterval", { interval: 100 });
      await cdp.send("Profiler.start");
      await page.evaluate(() => {
        const m = (window as any).__test.machine;
        for (let i = 0; i < 20; i++) m.runCycles(20_000_000);
      });
      const { profile } = await cdp.send("Profiler.stop");
      const self = new Map<string, number>();
      const byId = new Map<number, any>();
      for (const n of profile.nodes) byId.set(n.id, n);
      const counts = new Map<number, number>();
      for (const s of profile.samples ?? []) counts.set(s, (counts.get(s) ?? 0) + 1);
      let total = 0;
      for (const [id, c] of counts) {
        const n = byId.get(id);
        const name = n?.callFrame?.functionName || "(anonymous)";
        self.set(name, (self.get(name) ?? 0) + c);
        total += c;
      }
      const rows = [...self.entries()].sort((a, b) => b[1] - a[1]).slice(0, 40);
      console.log("PROFILE total samples " + total);
      for (const [name, c] of rows)
        console.log(`  ${((c / total) * 100).toFixed(2).padStart(6)}%  ${name}`);
    }

    // Raw interpreter throughput: synchronous runCycles windows.
    const raw: number[] = await page.evaluate(() => {
      const m = (window as any).__test.machine;
      const out: number[] = [];
      for (let i = 0; i < 6; i++) {
        const c0 = m.totalCycles(), t0 = performance.now();
        m.runCycles(20_000_000);
        const dt = performance.now() - t0;
        out.push((m.totalCycles() - c0) / dt / 1000);
      }
      return out;
    });
    const mean = raw.reduce((a, b) => a + b, 0) / raw.length;
    console.log(
      `BENCH mode=${realtime ? "realtime" : "fast"} paced=${paced.toFixed(1)} M/s   ` +
        `raw=[${raw.map((r) => r.toFixed(1)).join(" ")}] mean=${mean.toFixed(1)} M/s`,
    );
  });
});
