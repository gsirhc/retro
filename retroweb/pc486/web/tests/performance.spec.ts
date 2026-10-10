import { test, expect } from "./fixtures";
import { bootLive } from "./helpers";

// Tier 1 (clock, dropped cycles, main-thread share, emul/draw split, fps, memory) is host-side
// timing and works on the shipped binary. Tier 2 is the emulator's own counters, only in the
// instrumented build (pc486-perf, PERF=1). ?perf asks for it and falls back if absent.
test.describe("Performance panel", () => {
  test("is absent without ?perf", async ({ livePage: page }) => {
    await expect(page.locator("#perfCard")).toBeHidden();
  });

  test("the audio block reports ring health, pump cadence and pacing", async ({ perfPage: page }) => {
    // Audio breaking up while the clock holds 100%: the ring is fed from the main thread, so its depth,
    // the pump's cadence spread and whether the guest has produced the audio separate the causes.
    await page.locator("#speakerEnabled").check();
    const out = page.locator("#perfReadout");
    await expect(out).toContainText("ring", { timeout: 15_000 });
    await expect(out).toContainText("post", { timeout: 15_000 });
    await expect(out).toContainText("pace", { timeout: 15_000 });
    // Counters reset every second, so wait for a full window after audio is switched on.
    await expect
      .poll(async () => {
        const t = (await out.textContent())!;
        return Number(/post\s+(\d+)\/s/.exec(t)?.[1] ?? 0);
      }, { timeout: 20_000 })
      .toBeGreaterThan(0);
    const text = (await out.textContent())!;
    for (const want of ["starved", "trimmed", "fed", "p50", "p95", "max",
                        "guest", "fm", "of real time", "range"]) {
      expect(text).toContain(want);
    }
    const p50 = Number(/p50 ([\d.]+) ms/.exec(text)![1]);
    expect(p50).toBeGreaterThan(0);
    expect(p50).toBeLessThan(500);
    // At real speed the guest produces about a second of audio per second.
    const guest = Number(/guest ([\d.]+)x/.exec(text)![1]);
    if (!process.env.SLOW_HOST) expect(guest).toBeGreaterThan(0.2);
    // The range shows a brief surplus or drain the correction already bled off by the snapshot.
    // min <= current <= max always.
    const [, rMin, rMax] = /range (\d+)-(\d+) ms \(60s\)/.exec(text)!;
    const ring = Number(/ring\s+(\d+) ms of/.exec(text)![1]);
    expect(Number(rMin)).toBeLessThanOrEqual(ring);
    expect(Number(rMax)).toBeGreaterThanOrEqual(ring);
  });

  test("a third chart tracks the audio ring", async ({ perfPage: page }) => {
    const chart = page.locator("#perfChart3");
    await expect(chart).toBeVisible();
    // It has to paint: a blank canvas would hide the dropout.
    await page.locator("#speakerEnabled").check();
    await expect
      .poll(async () => await page.evaluate(() => {
        const c = document.getElementById("perfChart3") as HTMLCanvasElement;
        const d = c.getContext("2d")!.getImageData(0, 0, c.width, c.height).data;
        const seen = new Set<string>();
        for (let i = 0; i < d.length; i += 4) seen.add(d[i] + "," + d[i + 1] + "," + d[i + 2]);
        return seen.size;
      }), { timeout: 20_000 })
      .toBeGreaterThan(2);
  });

  test("?perf shows Tier 1 host metrics and the chart", async ({ perfPage: page }) => {
    const card = page.locator("#perfCard");
    await expect(card).toBeVisible();

    const out = page.locator("#perfReadout");
    await expect(out).toContainText("MHz of 66.0", { timeout: 15_000 });
    const text = (await out.textContent())!;
    for (const want of ["clock", "dropped", "486 cpu", "busy", "idle", "host",
                        "of one core", "emul", "draw", "fps", "heap"]) {
      expect(text).toContain(want);
    }
    const fps = Number(/([\d.]+) fps/.exec(text)![1]);
    expect(fps).toBeGreaterThan(1);
    expect(fps).toBeLessThan(200);
    const host = Number(/host\s+(\d+)% of one core/.exec(text)![1]);
    expect(host).toBeGreaterThanOrEqual(0);
    expect(host).toBeLessThanOrEqual(100);
    // Busy vs halted from real halt cycles. Bare DOS busy-waits, so busy is high at the prompt; the
    // two must be real complementary percentages.
    const guest = Number(/486 cpu\s+(\d+)% busy/.exec(text)![1]);
    const idle = Number(/busy\s+(\d+)% idle/.exec(text)![1]);
    expect(guest).toBeGreaterThanOrEqual(0);
    expect(guest).toBeLessThanOrEqual(100);
    // Shares of cycles that elapsed, so they sum to 100, not to a fixed 66MHz (the harness clock is faster).
    expect(guest + idle).toBeGreaterThanOrEqual(99);
    expect(guest + idle).toBeLessThanOrEqual(101);
    // The heap holds the 32MB of guest RAM, so it is never zero.
    const heap = Number(/heap\s+(\d+) MB/.exec(text)![1]);
    expect(heap).toBeGreaterThan(16);

    // Audio has its own line regardless of sound: the ring running dry happens on the worklet thread,
    // invisible to the emulator's counters.
    expect(text).toContain("audio");

    // "host" divides the browser's numbers from the machine's.
    const html = await page.locator("#perfReadout").innerHTML();
    expect(html).toMatch(/<b>host<\/b>/);

    const painted = await page.evaluate(() =>
      ["perfChart", "perfChart2"].map((id) => {
        const c = document.getElementById(id) as HTMLCanvasElement;
        const d = c.getContext("2d")!.getImageData(0, 0, c.width, c.height).data;
        const seen = new Set<string>();
        for (let i = 0; i < d.length; i += 4) seen.add(d[i] + "," + d[i + 1] + "," + d[i + 2]);
        return seen.size;
      }));
    expect(painted[0]).toBeGreaterThan(2);
    expect(painted[1]).toBeGreaterThan(2);
  });

  // If the instrumented binary is absent, ?perf falls back to the shipped one and keeps the host half.
  test("falls back to the shipped binary when the instrumented one is missing", async ({
    page,
  }) => {
    await page.route("**/pc486-perf.js", (route) => route.abort());
    // The aborted fetch plus retry costs a beat.
    test.setTimeout(120_000);
    await bootLive(page, { params: "perf=1" });
    await expect(page.locator("#perfCard")).toBeVisible();
    const out = page.locator("#perfReadout");
    await expect(out).toContainText("MHz of 66.0", { timeout: 15_000 });
    const text = (await out.textContent())!;
    expect(text).toContain("cpu");
    expect(text).not.toContain("cyc/instr");
    await expect(page.locator("#perfTier")).toContainText("Shipped build");
  });

  // Real speed on purpose, see tone.spec.ts: the lead figure needs a guest pacing to wall time.
  test("reports the audio ring's health once sound is enabled", async ({ page }) => {
    test.setTimeout(60000);
    await bootLive(page, { params: "perf=1", realtime: true });
    // Needs a running AudioContext, and a parallel run starts three at once. Tripling the budget
    // beats weakening the assertions.
    test.slow();
    await page.locator("#speakerEnabled").check();
    // Wait for the context to be running (see speaker.spec.ts); asserting earlier races the
    // worklet's first stats message.
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.audioState), { timeout: 30_000 })
      .toBe("running");
    const out = page.locator("#perfReadout");
    await expect(out).toContainText("ring", { timeout: 30_000 });
    const text = (await out.textContent())!;
    // Depth, starvation and what the pump feeds. The target comes with the stats, so follow it
    // instead of pinning a literal.
    expect(text).toMatch(/ring\s+\d+ ms of \d+ target/);
    expect(text).toMatch(/starved\s+[\d.]+ ms\/s/);
    expect(text).toMatch(/fed\s+[\d.]+k\/s/);
    // `fed` is the card's stamped samples at the rate it produces them: zero when silent, ~49.7k/s
    // with the OPL3 playing. The first window after sound is on covers part of a second.
    await page.evaluate(() => {
      const m = (window as any).__test.machine;
      const w = (r: number, v: number) => { m.portOut(0x388, r); m.portOut(0x389, v); };
      w(0x01, 0x20); w(0x20, 0x21); w(0x23, 0x21); w(0x40, 0x3F); w(0x43, 0x00);
      w(0x60, 0xFF); w(0x63, 0xF0); w(0x80, 0x00); w(0x83, 0x00);
      w(0xC0, 0x31); w(0xA0, 0x98); w(0xB0, 0x31);
    });
    await expect
      .poll(async () => {
        const t = (await out.textContent())!;
        const m = /fed\s+([\d.]+)k\/s/.exec(t);
        return m ? Number(m[1]) : 0;
      }, { timeout: 20_000 })
      .toBeGreaterThan(20);
    const fedNow = Number(/fed\s+([\d.]+)k\/s/.exec((await out.textContent())!)![1]);
    expect(fedNow).toBeLessThan(120);
  });

  test("Tier 2 follows the loaded binary's own perfBuild() answer", async ({ perfPage: page }) => {
    await expect(page.locator("#perfReadout")).toContainText("MHz of 66.0", { timeout: 15_000 });
    const tier2 = await page.evaluate(() => {
      const m = (window as any).__test.machine;
      return typeof m.perfBuild === "function" && m.perfBuild();
    });
    const text = (await page.locator("#perfReadout").textContent())!;
    const label = (await page.locator("#perfTier").textContent())!;

    if (tier2) {
      expect(label).toContain("Instrumented build");
      expect(text).toContain("cyc/instr");
      expect(text).toContain("hot instructions");
      // Shares must be real percentages; 0F is an escape byte, not an instruction, so never listed.
      const shares = Array.from(text.matchAll(/^\s+([\d.]+)%/gm)).map((x) => Number(x[1]));
      expect(shares.length).toBeGreaterThan(0);
      expect(shares.reduce((a, b) => a + b, 0)).toBeLessThanOrEqual(100.5);
      expect(text).toMatch(/\b(mov|cmp|jmp|push|pop|add|sub|xor|pfx|lea|test|ret)\b/);
      expect(text).not.toMatch(/^\s+[\d.]+%\s+0F\s+0F\s*$/m);
    } else {
      expect(label).toContain("Shipped build");
      expect(text).not.toContain("cyc/instr");
      expect(text).toContain("clock");
    }
  });
});
