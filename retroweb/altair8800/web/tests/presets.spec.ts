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

  test("switching presets leaves no standing note", async ({ page }) => {
    await boot(page, { params: "preset=cassette" });
    for (const id of ["cpm", "baremetal", "stock"]) {
      await page.selectOption("#preset", id);
      await page.waitForFunction(() => (window as any).__test?.applyingPreset === false);
      await expect(page.locator("#presetNote"), id).toHaveText("");
    }
  });

  test("autoload OFF (the ?test default) configures hardware only", async ({ page }) => {
    await boot(page, { params: "preset=cassette" });
    expect(await page.locator("#autoload").isChecked()).toBe(false);
    // hardware is configured but nothing was streamed into the machine
    await page.waitForTimeout(500);
    expect(await page.evaluate(() => (window as any).__test.screen())).not.toMatch(/MEMORY SIZE/i);
    expect(await page.evaluate(() => (window as any).__test.tape.phase)).toBe("idle");
  });

  test("ticking Auto-load streams the preset's software", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await page.check("#autoload");
    // 4K BASIC cold-starts and its prompts are answered -> OK
    await waitForScreen(page, /\bOK\b/, 40_000);
  });

  test("Auto-load on the CP/M preset boots the disk to A>", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await page.check("#autoload");
    await waitForScreen(page, /A>/, 30_000);
  });

  test("a preset applied with Auto-load already remembered boots its software", async ({ page }) => {
    // first visit: tick Auto-load so it's stored
    await boot(page);
    await page.evaluate(() => localStorage.setItem("retro8080.autoload", "1"));
    // second visit: the preset applies with autoload on -> software loads itself
    await page.goto("/?test=1&preset=cpm");
    await page.waitForFunction(() => !!(window as any).__test?.machine);
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
    await page.route("**/roms/8kbas.bin", (r) => r.abort());
    await page.goto("/?test=1&preset=cassette");
    await page.waitForFunction(() => (window as any).__test?.applyingPreset === false);
    await page.check("#autoload");
    await expect(page.locator("#presetNote")).toContainText(/could not be loaded|not.*load/i, {
      timeout: 10_000,
    });
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.screen()))
      .toMatch(/didn't load|could not be loaded/i);
    // the cassette deck (the other device) is still fitted
    await expect(page.locator("#acr")).not.toHaveClass(/empty/);
  });
});
