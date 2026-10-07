import { test, expect } from "@playwright/test";

test.describe("terminal", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/");
    await expect(page.locator("#screen .xterm-rows")).toContainText("\\", { timeout: 45000 });
  });

  test("defaults to the Modern (xterm) profile, no CRT housing", async ({ page }) => {
    await expect(page.locator("#termProfile")).toHaveValue("modern");
    await expect(page.locator("#monitor")).not.toHaveClass(/crt/);
  });

  test("every period profile is selectable and re-themes the screen", async ({ page }) => {
    for (const value of ["vt100g", "vt100a", "vt52", "adm3a", "glasstty", "tty33", "modern"]) {
      await page.selectOption("#termProfile", value);
      await expect(page.locator("#termProfile")).toHaveValue(value);
    }
    // CRT profiles get the monitor housing; Modern doesn't.
    await page.selectOption("#termProfile", "vt100g");
    await expect(page.locator("#monitor")).toHaveClass(/crt/);
    await page.selectOption("#termProfile", "modern");
    await expect(page.locator("#monitor")).not.toHaveClass(/crt/);
  });

  test("baud label reflects the ACIA's live configured rate", async ({ page }) => {
    // bios.s RESET programs 19200 baud (ACIA_CTRL = $1F)
    await expect(page.locator("#baudLabel")).toHaveText("19200 baud");
  });

  test.describe("CAPS LOCK", () => {
    // Spy on Machine.typeChar to prove the terminal itself uppercases, not the
    // firmware's FORCE_UPPER. Bytes arrive via app.js's input-pacing queue, so poll.
    async function typedBytes(page: import("@playwright/test").Page, text: string) {
      await page.evaluate(() => {
        (window as any).__typed = [];
        const m = (window as any).__machine;
        const orig = m.typeChar.bind(m);
        m.typeChar = (b: number) => { (window as any).__typed.push(b); return orig(b); };
      });
      await page.click("#screen");
      await page.keyboard.type(text);
      await expect.poll(() => page.evaluate(() => (window as any).__typed.length), { timeout: 20000 })
        .toBeGreaterThanOrEqual(text.length);
      return page.evaluate(() => (window as any).__typed as number[]);
    }

    test("is checked by default and uppercases typed letters", async ({ page }) => {
      await expect(page.locator("#caps")).toBeChecked();
      const bytes = await typedBytes(page, "ab");
      expect(bytes).toEqual(["A".charCodeAt(0), "B".charCodeAt(0)]);
    });

    test("unchecking it sends lowercase through unmodified", async ({ page }) => {
      await page.uncheck("#caps");
      const bytes = await typedBytes(page, "ab");
      expect(bytes).toEqual(["a".charCodeAt(0), "b".charCodeAt(0)]);
    });

    test("is forced on and disabled for the mechanically-uppercase-only ASR-33 profile", async ({ page }) => {
      await page.uncheck("#caps");
      await page.selectOption("#termProfile", "tty33");
      await expect(page.locator("#caps")).toBeChecked();
      await expect(page.locator("#caps")).toBeDisabled();
      await page.selectOption("#termProfile", "modern");
      await expect(page.locator("#caps")).toBeEnabled();
    });
  });
});
