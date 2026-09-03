import { test, expect } from "./fixtures";
import { boot, send, screen } from "./helpers";

const cursorY = (page) =>
  page.evaluate(() => {
    const b = (window as any).__test.term.buffer.active;
    return b.baseY + b.cursorY;
  });

test.describe("terminal I/O options", () => {
  test("CR -> CR/LF: a bare CR advances a line only when the box is checked", async ({ page }) => {
    await boot(page); // echo ROM
    await page.check("#crlf");
    const y0 = await cursorY(page);
    await send(page, "X\rY");
    await expect.poll(() => screen(page)).toMatch(/X\s*\n\s*Y/);
    expect(await cursorY(page)).toBeGreaterThan(y0);

    await page.uncheck("#crlf");
    const y1 = await cursorY(page);
    await send(page, "P\rQ"); // CR just returns the carriage; Q overprints P
    await page.waitForTimeout(200);
    expect(await cursorY(page)).toBe(y1);
  });

  test("CAPS LOCK off lets lowercase through", async ({ page }) => {
    await boot(page); // echo ROM
    await page.uncheck("#caps");
    await page.evaluate(() => (window as any).__test.term.focus());
    await page.keyboard.type("abcDEF", { delay: 10 });
    await expect.poll(() => screen(page)).toMatch(/abcDEF/);
  });

  test("the terminal bell flashes the bezel, then clears", async ({ page }) => {
    await boot(page); // echo ROM echoes the BEL back -> xterm fires onBell
    // the flash class lasts only 90 ms -- watch for it with an observer
    await page.evaluate(() => {
      (window as any).__flashed = false;
      const b = document.querySelector(".bezel")!;
      new MutationObserver(() => {
        if (b.classList.contains("bell-flash")) (window as any).__flashed = true;
      }).observe(b, { attributes: true, attributeFilter: ["class"] });
    });
    await send(page, "\x07");
    await expect.poll(() => page.evaluate(() => (window as any).__flashed)).toBe(true);
    await expect(page.locator(".bezel")).not.toHaveClass(/bell-flash/, { timeout: 2000 });
  });

  test("the Teletype bell takes the audible path (bellMode 'ding')", async ({ page }) => {
    // spy on AudioContext so we can prove the non-flash branch ran
    await page.addInitScript(() => {
      const Real = window.AudioContext || (window as any).webkitAudioContext;
      (window as any).__toneCount = 0;
      class Spy extends Real {
        createOscillator() {
          (window as any).__toneCount++;
          return super.createOscillator();
        }
      }
      window.AudioContext = Spy as any;
      (window as any).webkitAudioContext = Spy;
    });
    await boot(page, { params: "term=tty33" }); // tty33 profile sets bell: "ding"
    await send(page, "\x07");
    await expect.poll(() => page.evaluate(() => (window as any).__toneCount)).toBeGreaterThan(0);
  });
});

test.describe("load-device chips (custom build)", () => {
  test("each chip shows/hides its device and persists", async ({ page }) => {
    await boot(page);
    await page.selectOption("#preset", "");
    await expect(page.locator("#deviceChips")).toBeVisible();

    await page.check('#deviceChips input[data-dev="papertape"]');
    await expect(page.locator("#ptr")).not.toHaveClass(/empty/);
    await page.check('#deviceChips input[data-dev="cassette"]');
    await expect(page.locator("#acr")).not.toHaveClass(/empty/);
    await page.uncheck('#deviceChips input[data-dev="papertape"]');
    await expect(page.locator("#ptr")).toHaveClass(/empty/);

    const stored = await page.evaluate(() => localStorage.getItem("retro8080.devices"));
    expect(stored).toContain("cassette");
    expect(stored).not.toContain("papertape");
  });
});

test.describe("?debug seam", () => {
  test("?debug exposes window.__dbg (separate from the test seam)", async ({ page }) => {
    await page.goto("/?debug");
    await page.waitForFunction(() => !!(window as any).__dbg);
    const d = await page.evaluate(() => ({
      hasScreen: typeof (window as any).__dbg.screen === "string",
      preset: (window as any).__dbg.preset,
      ram: (window as any).__dbg.ram,
      pc: (window as any).__dbg.pc,
    }));
    expect(d.hasScreen).toBe(true);
    expect(typeof d.ram).toBe("number");
    expect(typeof d.pc).toBe("number");
    // no ?test -> no __test
    expect(await page.evaluate(() => (window as any).__test)).toBeUndefined();
  });
});
