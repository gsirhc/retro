import { test, expect } from "./fixtures";
import { boot, send, screen, regs, waitForScreen } from "./helpers";

// Executable realism checks. ?test runs media loads at Max, but the CPU clock and terminal baud are never sped up.

test.describe("timing fidelity", () => {
  test("the 8080 runs at ~2 MHz of emulated time per wall-clock second", async ({ page }) => {
    await boot(page); // echo ROM spinning in its poll loop
    const c0 = (await regs(page)).cycles;
    const t0 = Date.now();
    await page.waitForTimeout(1000);
    const c1 = (await regs(page)).cycles;
    const dt = (Date.now() - t0) / 1000;
    const hz = (c1 - c0) / dt;
    expect(hz).toBeGreaterThan(1_700_000);
    expect(hz).toBeLessThan(2_300_000);
  });

  test("an ASR-33 (tty33) meters output at ~10 characters/second", async ({ page }) => {
    await boot(page, { params: "term=tty33" });
    // echo ROM: send 60 chars, watch them trickle onto the paper roll
    await send(page, "ABCDEFGHIJ".repeat(6));
    await page.waitForTimeout(1200);
    const mid = (await screen(page)).replace(/\s/g, "").length;
    expect(mid).toBeGreaterThan(3);
    expect(mid).toBeLessThan(30); // nowhere near all 60 yet
    await expect
      .poll(() => screen(page).then((s) => s.replace(/\s/g, "").length), { timeout: 12_000 })
      .toBeGreaterThanOrEqual(60);
  });

  test("the Modern terminal does not meter output", async ({ page }) => {
    await boot(page, { params: "term=modern" });
    await send(page, "ABCDEFGHIJ".repeat(6));
    await expect
      .poll(() => screen(page).then((s) => s.replace(/\s/g, "").length), { timeout: 3000 })
      .toBeGreaterThanOrEqual(60);
  });

  // ALTAIR_REVIEW.md §6: every profile's baud rate gets a metering assertion
  test.describe("every other CRT profile meters output near its real baud, not instantly", () => {
    const BAUD: Record<string, number> = { vt100g: 9600, vt100a: 9600, vt52: 4800, adm3a: 9600, glasstty: 300 };
    for (const [key, baud] of Object.entries(BAUD)) {
      test(`${key} (${baud} baud)`, async ({ page }) => {
        await boot(page, { params: `term=${key}` });
        const text = "ABCDEFGHIJ".repeat(30); // 300 chars -- well under one screen, no scroll-loss risk
        await send(page, text);
        await page.waitForTimeout(150);
        const mid = (await screen(page)).replace(/\s/g, "").length;
        expect(mid, key).toBeLessThan(text.length); // hasn't all landed instantly
        await expect
          .poll(() => screen(page).then((s) => s.replace(/\s/g, "").length), { timeout: 15_000 })
          .toBeGreaterThanOrEqual(text.length);
      });
    }
  });

  test("paper tape at 'Realistic' feeds ~10 bytes/second", async ({ page }) => {
    await boot(page, { params: "preset=cassette" });
    await page.evaluate(() => (window as any).__test.paperTape.setSpeed("realistic"));
    await page.click("#ptr .ptr-window");
    await page.waitForFunction(
      () =>
        [...document.querySelectorAll("#ptrList option")].some((o) =>
          /8K BASIC 4\.0/.test(o.textContent || ""),
        ),
    );
    await page.evaluate(() => {
      const sel = document.querySelector<HTMLSelectElement>("#ptrList")!;
      const i = [...sel.options].findIndex((o) => /8K BASIC 4\.0/.test(o.textContent || ""));
      sel.value = String(sel.options[i].value);
      sel.dispatchEvent(new Event("change"));
    });
    await page.click("#ptrThread");
    await page.click("#ptr .ptr-load");

    const fed = () =>
      page.evaluate(() => {
        const t = (window as any).__test.tape;
        return t.lead + t.pos;
      });
    const t0 = Date.now();
    await page.waitForTimeout(3000);
    const bytes = await fed();
    const rate = bytes / ((Date.now() - t0) / 1000);
    expect(rate).toBeGreaterThan(3);
    expect(rate).toBeLessThan(30); // definitely not the whole 8 KB
  });

  // ALTAIR_REVIEW.md §3.2d: Realistic gates IN 0x09's sector advance to ~193/sec (~166 ms/rev).
  // ?test forces Max; this test selects Realistic to prove the throttle. A CP/M cold boot takes
  // ~1.8 s of CPU alone, so the floor sits well above that.
  test("disk at 'Realistic' measurably slows a CP/M boot", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await page.evaluate(() => (window as any).__test.disk.setSpeed("realistic"));
    const t0 = Date.now();
    await page.click("#dcdd .dcdd-boot");
    await waitForScreen(page, /A>/, 30_000);
    expect(Date.now() - t0).toBeGreaterThan(2200);
  });

  test("paper tape at 'Max' feeds the whole image near-instantly", async ({ page }) => {
    await boot(page, { params: "preset=cassette" }); // ?test forces Max
    await page.click("#ptr .ptr-window");
    await page.waitForFunction(() =>
      [...document.querySelectorAll("#ptrList option")].some((o) =>
        /8K BASIC 4\.0/.test(o.textContent || ""),
      ),
    );
    await page.evaluate(() => {
      const sel = document.querySelector<HTMLSelectElement>("#ptrList")!;
      const i = [...sel.options].findIndex((o) => /8K BASIC 4\.0/.test(o.textContent || ""));
      sel.value = String(sel.options[i].value);
      sel.dispatchEvent(new Event("change"));
    });
    await page.click("#ptrThread");
    await page.click("#ptr .ptr-load");
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.tape.pos), { timeout: 4000 })
      .toBeGreaterThan(8000);
  });

  test("the load-time turbo does not survive a keypress", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await page.click("#ptr .ptr-load"); // cold-starts 4K BASIC with turbo on
    await page.waitForTimeout(500);
    await send(page, " "); // a byte from the "keyboard"
    // once BASIC is at OK the loader has released turbo; the clock is back to 2 MHz
    await expect
      .poll(async () => {
        const c0 = (await regs(page)).cycles;
        await page.waitForTimeout(300);
        return ((await regs(page)).cycles - c0) / 0.3;
      }, { timeout: 40_000 })
      .toBeLessThan(2_400_000);
  });
});
