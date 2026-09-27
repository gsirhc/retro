import { test, expect } from "./fixtures";
import { bootLive } from "./helpers";

// The Performance panel has two tiers. Tier 1 -- clock, dropped cycles, the
// main thread's share of a core, the emul/draw split, fps, memory -- is pure
// host-side timing and works against the shipped binary. Tier 2 is the
// emulator's own counters, which only the instrumented build (pc486-perf,
// built with PERF=1) carries, because they sit on the hottest paths there
// are. ?perf asks for the instrumented build and falls back if it is absent.
test.describe("Performance panel", () => {
  test("is absent without ?perf", async ({ page }) => {
    await bootLive(page);
    await expect(page.locator("#perfCard")).toBeHidden();
  });

  test("?perf shows Tier 1 host metrics and the chart", async ({ page }) => {
    await bootLive(page, { params: "perf=1" });
    const card = page.locator("#perfCard");
    await expect(card).toBeVisible();

    const out = page.locator("#perfReadout");
    await expect(out).toContainText("MHz of 66.0", { timeout: 15_000 });
    const text = (await out.textContent())!;
    for (const want of ["clock", "dropped", "486 cpu", "busy", "idle", "host",
                        "of one core", "emul", "draw", "fps", "heap"]) {
      expect(text).toContain(want);
    }
    // fps and the core share have to be real measurements.
    const fps = Number(/([\d.]+) fps/.exec(text)![1]);
    expect(fps).toBeGreaterThan(1);
    expect(fps).toBeLessThan(200);
    const host = Number(/host\s+(\d+)% of one core/.exec(text)![1]);
    expect(host).toBeGreaterThanOrEqual(0);
    expect(host).toBeLessThanOrEqual(100);
    // The 486's own usage: busy vs halted, from real halt cycles. Bare DOS
    // busy-waits rather than halting, so at the prompt this is genuinely
    // high -- what must hold is that it is a real percentage, and that busy
    // and halted are complements rather than two unrelated numbers.
    const guest = Number(/486 cpu\s+(\d+)% busy/.exec(text)![1]);
    const idle = Number(/busy\s+(\d+)% idle/.exec(text)![1]);
    expect(guest).toBeGreaterThanOrEqual(0);
    expect(guest).toBeLessThanOrEqual(100);
    // Both are shares of the cycles that actually elapsed, so they are
    // complements and must add to 100 -- not to a fixed 66MHz, which would
    // be wrong the moment the test harness's faster clock is in play.
    expect(guest + idle).toBeGreaterThanOrEqual(99);
    expect(guest + idle).toBeLessThanOrEqual(101);
    // The emulator's heap is where the 32MB of guest RAM lives, so it can
    // never read as zero.
    const heap = Number(/heap\s+(\d+) MB/.exec(text)![1]);
    expect(heap).toBeGreaterThan(16);

    // Audio has its own line whether or not sound is enabled -- the ring
    // running dry is the one failure the emulator's own counters cannot see,
    // since it happens on the worklet thread.
    expect(text).toContain("audio");

    // "host" is emphasised: it divides the browser's numbers from the
    // machine's, which is exactly the distinction that gets misread.
    const html = await page.locator("#perfReadout").innerHTML();
    expect(html).toMatch(/<b>host<\/b>/);

    // Both charts have to actually paint, not merely exist.
    const painted = await page.evaluate(() =>
      ["perfChart", "perfChart2"].map((id) => {
        const c = document.getElementById(id) as HTMLCanvasElement;
        const d = c.getContext("2d")!.getImageData(0, 0, c.width, c.height).data;
        const seen = new Set<string>();
        for (let i = 0; i < d.length; i += 4) seen.add(d[i] + "," + d[i + 1] + "," + d[i + 2]);
        return seen.size;
      }));
    expect(painted[0]).toBeGreaterThan(2);   // 486 cpu + clock over grid
    expect(painted[1]).toBeGreaterThan(2);   // host core + fps over grid
  });

  // If the instrumented binary is absent -- a deploy that never ran
  // PERF=1 -- ?perf must fall back to the shipped one and still give the
  // host-side half, rather than failing to boot at all.
  test("falls back to the shipped binary when the instrumented one is missing", async ({
    page,
  }) => {
    await page.route("**/pc486-perf.js", (route) => route.abort());
    // The aborted fetch plus the retry costs a beat, so allow for it.
    test.setTimeout(120_000);
    await bootLive(page, { params: "perf=1" });
    await expect(page.locator("#perfCard")).toBeVisible();
    const out = page.locator("#perfReadout");
    await expect(out).toContainText("MHz of 66.0", { timeout: 15_000 });
    const text = (await out.textContent())!;
    expect(text).toContain("cpu");
    expect(text).not.toContain("cyc/instr");       // Tier 2 genuinely absent
    await expect(page.locator("#perfTier")).toContainText("Shipped build");
  });

  test("reports the audio ring's health once sound is enabled", async ({ page }) => {
    // Needs a genuinely running AudioContext, and a full parallel run has
    // three browsers starting one at once -- contention, not a slow metric.
    // Tripling the budget beats weakening the assertions, which are the
    // whole point: a test that passed whether or not audio started would
    // check nothing.
    test.slow();
    await bootLive(page, { params: "perf=1" });
    await page.locator("#speakerEnabled").check();
    // Wait for the context to be genuinely running, not merely created --
    // see speaker.spec.ts. Asserting on the ring before then races the
    // worklet's first stats message, which is what made this flaky under a
    // full parallel run.
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.audioState), { timeout: 30_000 })
      .toBe("running");
    const out = page.locator("#perfReadout");
    await expect(out).toContainText("ring", { timeout: 30_000 });
    const text = (await out.textContent())!;
    // Depth, starvation and what the pump feeds it -- the three that
    // together say whether audio broke up, and why.
    expect(text).toMatch(/ring\s+\d+ ms of 50 target/);
    expect(text).toMatch(/starved\s+[\d.]+ ms\/s/);
    expect(text).toMatch(/fed\s+[\d.]+k\/s/);
    // The pump must feed the ring at about the context's own rate, or the
    // ring drains (too little) or is trimmed away (too much). Poll for it:
    // the first sampling window after sound is switched on covers only the
    // part of that second the pump was actually feeding, so it reads low by
    // construction -- a real rate needs one whole window.
    await expect
      .poll(async () => {
        const t = (await out.textContent())!;
        const m = /fed\s+([\d.]+)k\/s/.exec(t);
        return m ? Number(m[1]) : 0;
      }, { timeout: 20_000 })
      .toBeGreaterThan(20);
    const fedNow = Number(/fed\s+([\d.]+)k\/s/.exec((await out.textContent())!)![1]);
    expect(fedNow).toBeLessThan(100);
  });

  test("Tier 2 follows the loaded binary's own perfBuild() answer", async ({ page }) => {
    await bootLive(page, { params: "perf=1" });
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
      // Shares must be real percentages, and 0F must not be double-counted:
      // the escape byte is not an instruction, so it must never be listed.
      const shares = Array.from(text.matchAll(/^\s+([\d.]+)%/gm)).map((x) => Number(x[1]));
      expect(shares.length).toBeGreaterThan(0);
      expect(shares.reduce((a, b) => a + b, 0)).toBeLessThanOrEqual(100.5);
      expect(text).toMatch(/\b(mov|cmp|jmp|push|pop|add|sub|xor|pfx|lea|test|ret)\b/);
      expect(text).not.toMatch(/^\s+[\d.]+%\s+0F\s+0F\s*$/m);
    } else {
      expect(label).toContain("Shipped build");
      expect(text).not.toContain("cyc/instr");
      expect(text).toContain("clock");   // Tier 1 still works
    }
  });
});
