import { test, expect } from "./fixtures";
import { boot } from "./helpers";

const profile = (page) =>
  page.evaluate(() => {
    const t = (window as any).__test.term;
    const screenEl = document.getElementById("screen")!;
    const bezel = screenEl.closest(".bezel")!;
    const monitor = screenEl.closest(".monitor")!;
    return {
      fontSize: t.options.fontSize,
      fontFamily: t.options.fontFamily,
      bezel: bezel.className,
      monitor: monitor.className,
      crtDisabled: (document.getElementById("crt") as HTMLInputElement).disabled,
      stored: localStorage.getItem("retro8080.term"),
    };
  });

async function selectTerm(page, key: string) {
  await page.selectOption("#termProfile", key);
  await page.waitForTimeout(150);
}

test.describe("terminal profiles", () => {
  const cases: [string, RegExp, number][] = [
    ["modern", /crt-none/, 15],
    ["vt100g", /crt-scan/, 19],
    ["vt100a", /crt-scan/, 19],
    ["vt52", /crt-scanheavy/, 19],
    ["adm3a", /crt-scan/, 19],
    ["glasstty", /crt-scan/, 19],
    ["tty33", /crt-paper/, 15],
  ];

  for (const [key, bezelRe, size] of cases) {
    test(`${key}: applyProfile sets font, bezel, and persists`, async ({ page }) => {
      await boot(page);
      await selectTerm(page, key);
      const p = await profile(page);
      expect(p.fontSize).toBe(size);
      expect(p.bezel).toMatch(bezelRe);
      expect(p.stored).toBe(key);
    });
  }

  test("modern profile disables the CRT toggle; a CRT profile enables it", async ({ page }) => {
    await boot(page);
    await selectTerm(page, "modern");
    expect((await profile(page)).crtDisabled).toBe(true);
    await selectTerm(page, "vt100g");
    expect((await profile(page)).crtDisabled).toBe(false);
  });

  test("vt100a adds the amber monitor class", async ({ page }) => {
    await boot(page);
    await selectTerm(page, "vt100a");
    expect((await profile(page)).monitor).toMatch(/amber/);
    await selectTerm(page, "vt100g");
    expect((await profile(page)).monitor).not.toMatch(/amber/);
  });

  test("Teletype and Modern use the 'bare' housing; Teletype scrolls", async ({ page }) => {
    await boot(page);
    await selectTerm(page, "tty33");
    let p = await profile(page);
    expect(p.monitor).toMatch(/bare/);
    expect(p.monitor).toMatch(/scrolls/);
    await selectTerm(page, "modern");
    p = await profile(page);
    expect(p.monitor).toMatch(/bare/);
    expect(p.monitor).not.toMatch(/scrolls/);
  });

  test("CRT toggle flips the crt-off class", async ({ page }) => {
    await boot(page);
    await selectTerm(page, "vt100g");
    expect((await profile(page)).bezel).not.toMatch(/crt-off/);
    await page.uncheck("#crt");
    await page.waitForTimeout(100);
    expect((await profile(page)).bezel).toMatch(/crt-off/);
    await page.check("#crt");
    await page.waitForTimeout(100);
    expect((await profile(page)).bezel).not.toMatch(/crt-off/);
  });

  test("CAPS LOCK forces uppercase into the machine", async ({ page }) => {
    await boot(page);
    await page.check("#caps");
    await page.evaluate(() => (window as any).__test.term.focus());
    await page.keyboard.type("abc", { delay: 12 });
    await expect.poll(() => page.evaluate(() => (window as any).__test.screen())).toMatch(/ABC/);
  });

  test("?term= selects the profile at load", async ({ page }) => {
    await boot(page, { params: "term=vt52" });
    expect((await profile(page)).bezel).toMatch(/crt-scanheavy/);
  });

  test("changing the terminal by hand flips the preset to custom", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    expect(await page.locator("#preset").inputValue()).toBe("cpm");
    await selectTerm(page, "vt52");
    expect(await page.locator("#preset").inputValue()).toBe("");
  });
});
