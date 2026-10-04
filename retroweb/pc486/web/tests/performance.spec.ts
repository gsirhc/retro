import { test, expect } from "./fixtures";
import { bootLive } from "./helpers";

// The Performance panel has two tiers. Tier 1 -- clock, dropped cycles, the
// main thread's share of a core, the emul/draw split, fps, memory -- is pure
// host-side timing and works against the shipped binary. Tier 2 is the
// emulator's own counters, which only the instrumented build (pc486-perf,
// built with PERF=1) carries, because they sit on the hottest paths there
// are. ?perf asks for the instrumented build and falls back if it is absent.
test.describe("Performance panel", () => {
  test("is absent without ?perf", async ({ livePage: page }) => {
    await expect(page.locator("#perfCard")).toBeHidden();
  });

  test("the audio block reports ring health, pump cadence and pacing", async ({ perfPage: page }) => {
    // Audio breaking up while the clock holds 100% is the case these lines
    // exist for: the ring is fed from the main thread, so its depth, the
    // spread of the pump's own cadence, and whether the guest has actually
    // produced the audio yet are the three things that separate the causes.
    await page.locator("#speakerEnabled").check();
    const out = page.locator("#perfReadout");
    await expect(out).toContainText("ring", { timeout: 15_000 });
    await expect(out).toContainText("post", { timeout: 15_000 });
    await expect(out).toContainText("pace", { timeout: 15_000 });
    // The counters reset every second, so the window in which audio was
    // switched on legitimately reports nothing -- wait for a full one.
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
    // At real speed the guest should be producing about a second of audio
    // per second; the panel is where a shortfall becomes visible.
    const guest = Number(/guest ([\d.]+)x/.exec(text)![1]);
    expect(guest).toBeGreaterThan(0.2);
    // The range is the one number the instant "ring N ms" reading can't
    // show: a brief surplus or drain that the correction already bled off
    // by the time this second's snapshot is read. min <= the current
    // reading <= max, always, since the current reading is one of the
    // samples the range is drawn from.
    const [, rMin, rMax] = /range (\d+)-(\d+) ms \(60s\)/.exec(text)!;
    const ring = Number(/ring\s+(\d+) ms of/.exec(text)![1]);
    expect(Number(rMin)).toBeLessThanOrEqual(ring);
    expect(Number(rMax)).toBeGreaterThanOrEqual(ring);
  });

  test("a third chart tracks the audio ring", async ({ perfPage: page }) => {
    const chart = page.locator("#perfChart3");
    await expect(chart).toBeVisible();
    // It has to actually paint, not just exist: a blank canvas would hide
    // exactly the dropout it is there to show.
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

  // Real speed on purpose -- see the note in tone.spec.ts on why the lead
  // figure only means something when the guest is pacing to wall time.
  test("reports the audio ring's health once sound is enabled", async ({ page }) => {
    test.setTimeout(60000);
    await bootLive(page, { params: "perf=1", realtime: true });
    // Needs a genuinely running AudioContext, and a full parallel run has
    // three browsers starting one at once -- contention, not a slow metric.
    // Tripling the budget beats weakening the assertions, which are the
    // whole point: a test that passed whether or not audio started would
    // check nothing.
    test.slow();
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
    // The target is the audio thread's own, reported with the stats rather
    // than written into the panel -- so this follows it instead of pinning a
    // literal that goes stale the moment the cushion is retuned.
    expect(text).toMatch(/ring\s+\d+ ms of \d+ target/);
    expect(text).toMatch(/starved\s+[\d.]+ ms\/s/);
    expect(text).toMatch(/fed\s+[\d.]+k\/s/);
    // `fed` is what the main thread hands the audio thread: the card's own
    // samples, stamped, at the rate the card produces them -- so it is zero
    // with the machine silent, and about 49.7k/s with the OPL3 playing (its
    // real output rate). Key a note on and poll: the first sampling window
    // after sound is switched on covers only part of a second.
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
