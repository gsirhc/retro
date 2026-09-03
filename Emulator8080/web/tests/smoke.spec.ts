import { test, expect } from "./fixtures";
import { boot, screen, send, pressKeys, regs } from "./helpers";

test.describe("smoke", () => {
  test("page boots, wasm instantiates, machine runs", async ({ page }) => {
    await boot(page);
    expect(await page.locator("#altair").isVisible()).toBe(true);
    expect(await page.locator("#screen .xterm").count()).toBeGreaterThan(0);
    const s = await regs(page);
    expect(typeof s.pc).toBe("number");
    expect(typeof s.cycles).toBe("number");
  });

  test("built-in echo ROM round-trips a byte fed to the serial port", async ({ page }) => {
    await boot(page);
    await send(page, "HELLO");
    await expect.poll(() => screen(page)).toContain("HELLO");
  });

  test("keyboard input reaches the emulator and echoes to the terminal", async ({ page }) => {
    await boot(page);
    await pressKeys(page, "AB12");
    await expect.poll(() => screen(page)).toMatch(/AB12/);
  });

  test("the test seam is absent without ?test", async ({ page }) => {
    await page.goto("/");
    await page.waitForTimeout(500);
    expect(await page.evaluate(() => (window as any).__test)).toBeUndefined();
  });

  test("?test forces media load speed to Max", async ({ page }) => {
    await boot(page);
    const speeds = await page.evaluate(() => ({
      tape: (window as any).__test.paperTape.speed,
      cass: (window as any).__test.cassette.speed,
    }));
    expect(speeds.tape).toBe("max");
    expect(speeds.cass).toBe("max");
  });

  test("fail() renders a red error screen when retro8080.js is unavailable", async ({ page }) => {
    await page.route("**/retro8080.js*", (r) => r.abort());
    await page.goto("/?test=1");
    await expect.poll(() => page.locator("#screen pre").innerText().catch(() => ""))
      .toMatch(/retro8080\.js did not load|did not load/i);
  });

  test("fail() when xterm.js is unavailable", async ({ page }) => {
    await page.route("**/vendor/xterm.js", (r) => r.abort());
    await page.goto("/?test=1");
    await expect.poll(() => page.locator("#screen pre").innerText().catch(() => ""))
      .toMatch(/xterm\.js did not load/i);
  });

  test("fail() when the xterm fit addon is unavailable", async ({ page }) => {
    await page.route("**/vendor/xterm-addon-fit.js", (r) => r.abort());
    await page.goto("/?test=1");
    await expect.poll(() => page.locator("#screen pre").innerText().catch(() => ""))
      .toMatch(/xterm-addon-fit did not load/i);
  });
});
