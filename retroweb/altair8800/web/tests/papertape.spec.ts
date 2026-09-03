import { test, expect } from "./fixtures";
import { boot, waitForScreen, screen, memAt, pickCatalogItem, hasCatalogItem } from "./helpers";

async function openReader(page) {
  await page.click("#ptr .ptr-window");
  await expect(page.locator("#ptrDialog")).toBeVisible();
}

async function threadAndLoad(page, label: RegExp) {
  await openReader(page);
  await pickCatalogItem(page, "#ptrList", label);
  await page.click("#ptrThread");
  await expect(page.locator("#ptr")).toHaveClass(/loaded/);
  await page.click("#ptr .ptr-load");
}

test.describe("paper-tape reader", () => {
  test("the reader dialog lists the ROM catalog", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await openReader(page);
    await expect
      .poll(() => page.locator("#ptrList option").count())
      .toBeGreaterThan(2);
    expect(await hasCatalogItem(page, "#ptrList", /8K BASIC 4\.0/)).toBe(true);
    expect(await hasCatalogItem(page, "#ptrList", /Kill the Bit/)).toBe(true);
  });

  test("BASIC listings are not offered as tapes", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await openReader(page);
    expect(await hasCatalogItem(page, "#ptrList", /Super Star Trek/)).toBe(false);
  });

  test("threading then LOAD boots 4K BASIC to OK (prompts auto-answered)", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await threadAndLoad(page, /4K BASIC 4\.0/);
    await waitForScreen(page, /MEMORY SIZE\?/i, 20_000);
    await waitForScreen(page, /\bOK\b/, 30_000);
  });

  test("threading then LOAD boots 8K BASIC to OK on a 32K machine", async ({ page }) => {
    await boot(page, { params: "preset=cassette" });
    await threadAndLoad(page, /8K BASIC 4\.0/);
    await waitForScreen(page, /\bOK\b/, 40_000);
    // BASIC actually runs
    await page.evaluate(() => {
      const m = (window as any).__test.machine;
      for (const c of 'PRINT 6*7\r') m.sendByte(c.charCodeAt(0));
    });
    await waitForScreen(page, /\b42\b/, 15_000);
  });

  test("8K BASIC is refused on a 4K machine (not enough RAM)", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await threadAndLoad(page, /8K BASIC 4\.0/);
    await expect(page.locator("#presetNote")).toContainText(/not enough RAM/i, {
      timeout: 8000,
    });
    expect(await screen(page)).not.toMatch(/MEMORY SIZE/i);
  });

  test("EJECT removes the tape and disables LOAD", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await openReader(page);
    await pickCatalogItem(page, "#ptrList", /4K BASIC 4\.0/);
    await page.click("#ptrThread");
    await expect(page.locator("#ptr")).toHaveClass(/loaded/);
    await page.click("#ptr .ptr-eject");
    await expect(page.locator("#ptr")).not.toHaveClass(/loaded/);
    await expect(page.locator("#ptr .ptr-load")).toBeDisabled();
  });

  test("the LOAD SPEED selector offers Realistic / 5x / 25x / 50x, and changes the rate live", async ({
    page,
  }) => {
    await boot(page, { params: "preset=cassette" });
    const opts = await page.$$eval("#ptr .ptr-speed option", (os) => os.map((o) => o.value));
    expect(opts).toEqual(["realistic", "x5", "x25", "x50"]);

    await page.selectOption("#ptr .ptr-speed", "realistic");
    await threadAndLoad(page, /8K BASIC 4\.0/);
    const fed = () => page.evaluate(() => (window as any).__test.tape.lead + (window as any).__test.tape.pos);

    // creeping in at ~10 B/s
    await page.waitForTimeout(1500);
    expect(await fed()).toBeLessThan(120);

    // flip to 50x mid-read -> the feed rate jumps to ~500 B/s
    await page.selectOption("#ptr .ptr-speed", "x50");
    const a = await fed();
    await page.waitForTimeout(2000);
    const rate = ((await fed()) - a) / 2;
    expect(rate).toBeGreaterThan(200); // nothing like the ~10 it was
  });

  test("a stale 'max' in retro8080.paperspeed falls back to Realistic", async ({ page }) => {
    await page.goto("/");
    await page.evaluate(() => localStorage.setItem("retro8080.paperspeed", "max"));
    await page.goto("/?preset=stock");
    await expect(page.locator("#ptr .ptr-speed")).toHaveValue("realistic");
  });

  test("the address lamps climb while a tape reads in", async ({ page }) => {
    await boot(page, { params: "preset=cassette" });
    await threadAndLoad(page, /8K BASIC 4\.0/);
    // during the feed the reader shows the 'reading' state
    await expect(page.locator("#ptr")).toHaveClass(/reading/, { timeout: 8000 });
    await waitForScreen(page, /\bOK\b/, 40_000);
    await expect(page.locator("#ptr")).not.toHaveClass(/reading/);
  });
});

test.describe("the reader's START button (authentic hand-load path)", () => {
  const slow = (page) =>
    page.evaluate(() => (window as any).__test.paperTape.setSpeed("realistic"));
  const org = 4 * 1024 - 0x40; // stock preset: bootstrap page = ramTop - 0x40

  test("START runs the reader without resetting the machine or keying a bootstrap", async ({
    page,
  }) => {
    await boot(page, { params: "preset=stock" }); // stock threads 4K BASIC already
    await expect(page.locator("#ptr")).toHaveClass(/loaded/);
    await slow(page);
    // a sentinel where AUTO-LOAD's bootstrap would land
    await page.evaluate((a) => (window as any).__test.machine.writeByte(a, 0xa5), org);

    await page.click("#ptr .ptr-start");
    await expect(page.locator("#ptr")).toHaveClass(/reading/);
    expect(await page.evaluate(() => (window as any).__test.tape.raw)).toBe(true);
    expect(await memAt(page, org)).toBe(0xa5); // untouched — no clear, no bootstrap
  });

  test("with a loader already running, START streams the tape into it", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await expect(page.locator("#ptr")).toHaveClass(/loaded/);
    // key the ~40-byte serial loader in by hand (the guide writes the bytes + sets PC)
    await page.click("#pgToggle");
    await page.click("#pgKeyin");
    // run the loader we just keyed in, then start the reader motor
    await page.evaluate(() => (window as any).__test.setRunning(true));
    await page.click("#ptr .ptr-start");
    // the hand-keyed loader catches the tape; 4K BASIC cold-starts.
    // it answers no prompts itself — MEMORY SIZE? is as far as it gets unaided.
    await waitForScreen(page, /MEMORY SIZE\?/i, 40_000);
  });

  test("the reader honours LOAD SPEED — Realistic really is ~10 B/s", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await expect(page.locator("#ptr")).toHaveClass(/loaded/);
    await slow(page); // override the ?test=1 Max
    await page.evaluate(() => (window as any).__test.setRunning(false)); // no consumer

    await page.click("#ptr .ptr-start");
    await expect(page.locator("#ptr")).toHaveClass(/reading/);
    const t0 = Date.now();
    await page.waitForTimeout(2500);
    const fed = await page.evaluate(() => {
      const t = (window as any).__test.tape;
      return t.lead + t.pos;
    });
    const rate = fed / ((Date.now() - t0) / 1000);
    expect(rate).toBeGreaterThan(4);
    expect(rate).toBeLessThan(25); // paced, not dumped — and the FIFO didn't freeze it
  });

  test("START with no loader overruns the 88-2SIO and still spools to the end", async ({ page }) => {
    await boot(page, { params: "preset=stock" }); // ?test => Max speed
    await expect(page.locator("#ptr")).toHaveClass(/loaded/);
    await page.evaluate(() => (window as any).__test.setRunning(false));

    await page.click("#ptr .ptr-start");
    // nothing is draining the FIFO, so the reader overruns it (bytes lost) and
    // runs the tape out — it must NOT freeze part-way.
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.tape.phase), { timeout: 15_000 })
      .toBe("idle");
    await expect(page.locator("#ptr")).not.toHaveClass(/reading/);
    expect(await screen(page)).not.toMatch(/MEMORY SIZE/i); // the bytes went nowhere
    expect(await page.evaluate(() => (window as any).__test.running)).toBe(false);
  });

  test("START toggles to STOP and halts the reader mid-tape", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await expect(page.locator("#ptr")).toHaveClass(/loaded/);
    await slow(page);

    await page.click("#ptr .ptr-start");
    await expect(page.locator("#ptr")).toHaveClass(/reading/);
    await expect(page.locator("#ptr .ptr-start")).toHaveText("STOP");
    // AUTO-LOAD and EJECT are locked while the reader runs
    await expect(page.locator("#ptr .ptr-load")).toBeDisabled();

    await page.click("#ptr .ptr-start"); // STOP
    await expect(page.locator("#ptr")).not.toHaveClass(/reading/);
    await expect(page.locator("#ptr .ptr-start")).toHaveText("START");
    expect(await page.evaluate(() => (window as any).__test.tape.phase)).toBe("idle");
  });

  test("AUTO-LOAD, by contrast, resets RAM and keys in its own bootstrap", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await expect(page.locator("#ptr")).toHaveClass(/loaded/);
    await page.evaluate((a) => (window as any).__test.machine.writeByte(a, 0xa5), org);

    await page.click("#ptr .ptr-load"); // AUTO-LOAD (?test forces Max speed)
    await expect.poll(() => memAt(page, org), { timeout: 8000 }).toBe(0x21); // LXI H — makeBootstrap
    expect(await page.evaluate(() => (window as any).__test.tape.raw)).toBe(false);
  });
});
