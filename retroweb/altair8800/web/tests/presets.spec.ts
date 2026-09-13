import { test, expect } from "./fixtures";
import { boot, waitForScreen } from "./helpers";

const shape = (page) =>
  page.evaluate(() => {
    const vis = (id: string) => !document.getElementById(id)!.classList.contains("empty");
    return {
      ram: (window as any).__test.machine.ramKb(),
      term: (document.getElementById("termProfile") as HTMLSelectElement).value,
      cards: [...document.querySelectorAll("#backplane .bp-card b")].map((b) => b.textContent),
      ptr: vis("ptr"),
      acr: vis("acr"),
      dcdd: vis("dcdd"),
      stored: localStorage.getItem("retro8080.preset"),
      note: document.getElementById("presetNote")!.textContent,
    };
  });

test.describe("era presets", () => {
  test("Bare-Metal Toggle: 4K, Teletype, paper tape only", async ({ page }) => {
    await boot(page, { params: "preset=baremetal" });
    const s = await shape(page);
    expect(s.ram).toBe(4);
    expect(s.term).toBe("tty33");
    expect(s.ptr).toBe(true);
    expect(s.acr).toBe(false);
    expect(s.dcdd).toBe(false);
    expect(s.cards.length).toBeGreaterThanOrEqual(3);
    expect(s.stored).toBe("baremetal");
  });

  test("Stock Launch: 4K, Teletype, paper tape only", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    const s = await shape(page);
    expect(s.ram).toBe(4);
    expect(s.term).toBe("tty33");
    expect(s.ptr).toBe(true);
    expect(s.acr).toBe(false);
  });

  test("Cassette Hobbyist: 32K, ADM-3A, paper tape + cassette", async ({ page }) => {
    await boot(page, { params: "preset=cassette" });
    const s = await shape(page);
    expect(s.ram).toBe(32);
    expect(s.term).toBe("adm3a");
    expect(s.ptr).toBe(true);
    expect(s.acr).toBe(true);
    expect(s.dcdd).toBe(false);
  });

  test("CP/M Workstation: 64K, VT100, floppy only", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    const s = await shape(page);
    expect(s.ram).toBe(64);
    expect(s.term).toBe("vt100g");
    expect(s.dcdd).toBe(true);
    expect(s.ptr).toBe(false);
    expect(s.acr).toBe(false);
  });

  // pins the era/hardware claims ALTAIR_REVIEW.md §5.1 flagged, so a future
  // edit that reintroduces an anachronism (or a fake MITS part number) fails
  // here instead of just in prose.
  test("preset eras and card lists match the corrected metadata", async ({ page }) => {
    await boot(page, { params: "preset=baremetal" });
    const presets = await page.evaluate(() => (window as any).__test.PRESETS);
    expect(presets.baremetal.era).toBe("1976");
    expect(presets.baremetal.cards).not.toContain("MITS 88-4K\nStatic RAM");
    expect(presets.baremetal.cards.join("|")).toMatch(/88-4MCS/);

    expect(presets.stock.era).toBe("1976");
    expect(presets.stock.cards.join("|")).toMatch(/88-4MCD/);

    expect(presets.cpm.era).toBe("1979");           // CP/M 2.2 shipped 1979
    expect(presets.cpm.cards.join("|")).not.toMatch(/3rd party/);
    expect(presets.cpm.cards.filter((c: string) => c.includes("88-16MCD")).length).toBe(4);
  });

  test("switching presets leaves no standing note", async ({ page }) => {
    await boot(page, { params: "preset=cassette" });
    for (const id of ["cpm", "baremetal", "stock"]) {
      await page.selectOption("#preset", id);
      await page.waitForFunction(() => (window as any).__test?.applyingPreset === false);
      await expect(page.locator("#presetNote"), id).toHaveText("");
    }
  });

  // A real Altair doesn't run anything on its own -- a preset threads the
  // software into its device (tape in the reader, diskette in the drive) but
  // never keys it in or presses RUN for you. boot() flips the front panel's
  // power switch on, which is as far as any preset goes by itself.
  test("presets configure hardware and thread media, but never load or boot on their own", async ({
    page,
  }) => {
    await boot(page, { params: "preset=cassette" });
    await page.waitForTimeout(500);
    expect(await page.evaluate(() => (window as any).__test.screen())).not.toMatch(/MEMORY SIZE/i);
    expect(await page.evaluate(() => (window as any).__test.tape.phase)).toBe("idle");
    expect(await page.evaluate(() => (window as any).__test.paperTape.entry)).not.toBe(null);
  });

  test("the reader's AUTO-LOAD button streams the preset's software", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await page.click("#ptr .ptr-load");
    // 4K BASIC cold-starts and its prompts are answered -> OK
    await waitForScreen(page, /\bOK\b/, 40_000);
  });

  test("the disk cabinet's BOOT button brings the CP/M preset up to A>", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await page.click("#dcdd .dcdd-boot");
    await waitForScreen(page, /A>/, 30_000);
  });

  test("preset choice persists across a reload", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine);
    await expect(page.locator("#preset")).toHaveValue("cpm");
    expect(await page.evaluate(() => (window as any).__test.machine.ramKb())).toBe(64);
  });

  test("the Guide dialog names the active preset", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await page.click("#presetGuide");
    await expect(page.locator("#presetGuideDialog")).toBeVisible();
    await expect(page.locator("#presetGuideName")).toHaveText(/CP\/M Workstation/);
    await expect(page.locator("#presetGuideBody")).toContainText(/A>/);
    await page.click("#presetGuideOk");
    await expect(page.locator("#presetGuideDialog")).toBeHidden();
  });

  test("selecting '— custom —' shows the load-device chips", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await expect(page.locator("#deviceChips")).toBeHidden();
    await page.selectOption("#preset", "");
    await expect(page.locator("#deviceChips")).toBeVisible();
  });

  test("a preset whose software can't be found sets the hardware and says so", async ({ page }) => {
    // block the flat load-and-go image AND its ROM-build alternative -- the
    // catalog falls back to whichever "8K BASIC" entry it can actually fetch,
    // so simulating "nothing available" means blocking both
    await page.route(/\/roms\/(8kbas\.bin|8kBas_.*\.bin)$/i, (r) => r.abort());
    await page.goto("/?test=1&preset=cassette");
    await expect(page.locator("#presetNote")).toContainText(/could not be loaded|not.*load/i, {
      timeout: 10_000,
    });
    // the cassette deck (the other device) is still fitted
    await expect(page.locator("#acr")).not.toHaveClass(/empty/);
  });
});
