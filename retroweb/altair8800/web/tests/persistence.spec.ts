import { test, expect } from "./fixtures";
import { boot, waitForScreen } from "./helpers";

test.describe("page chrome, persistence, URL params", () => {
  test("the page theme select drives data-theme and persists", async ({ page }) => {
    await boot(page);
    for (const t of ["web94", "modern", "win"]) {
      await page.selectOption("#pageTheme", t);
      await expect(page.locator("html")).toHaveAttribute("data-theme", t);
      expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe(t);
    }
  });

  test("?theme= wins at load", async ({ page }) => {
    await boot(page, { params: "theme=web94" });
    await expect(page.locator("html")).toHaveAttribute("data-theme", "web94");
  });

  test("Dark Modern = Modern layout + data-mode=dark, cleared when switching away", async ({ page }) => {
    await boot(page);
    await page.selectOption("#pageTheme", "moderndark");
    await expect(page.locator("html")).toHaveAttribute("data-theme", "modern");
    await expect(page.locator("html")).toHaveAttribute("data-mode", "dark");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe("moderndark");
    expect(await page.evaluate(() => getComputedStyle(document.body).backgroundColor)).toBe("rgb(18, 19, 23)");

    // restored on reload
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine);
    await expect(page.locator("html")).toHaveAttribute("data-mode", "dark");
    expect(await page.locator("#pageTheme").inputValue()).toBe("moderndark");

    // switching to another theme drops the dark mode
    await page.selectOption("#pageTheme", "win");
    await expect(page.locator("html")).toHaveAttribute("data-theme", "win");
    expect(await page.evaluate(() => document.documentElement.hasAttribute("data-mode"))).toBe(false);
  });

  test("the theme picker lives in the top bar, not a mid-page toolbar", async ({ page }) => {
    await boot(page);
    await expect(page.locator(".pagebar #pageTheme")).toBeVisible();
    await expect(page.locator(".toolbar #pageTheme")).toHaveCount(0);
    await expect(page.locator('.pagebar #pageTheme option[value="win"]')).toHaveText(/Windows 95/);
  });

  test("every theme offers a way back to the landing page", async ({ page }) => {
    await boot(page);
    for (const t of ["win", "web94", "modern"]) {
      await page.selectOption("#pageTheme", t);
      // win: the titlebar close box; web94/modern: the "All machines" link
      const home = page.locator('.pagebar a[href="../"]:visible');
      await expect(home).toHaveCount(1);
    }
  });

  test("every theme flows front panel -> terminal -> devices", async ({ page }) => {
    await page.setViewportSize({ width: 1200, height: 900 }); // below the side-by-side breakpoint
    for (const t of ["win", "web94", "modern"]) {
      await boot(page, { params: `theme=${t}&preset=cassette` });
      await expect(page.locator("#ptr")).toBeVisible();
      const y = async (s: string) => (await page.locator(s).boundingBox())!.y;
      const [panel, termBar, dev] =
        await Promise.all([y("#altair"), y(".term-bar"), y("#ptr")]);
      expect(panel, t).toBeLessThan(termBar);   // front panel above the terminal
      expect(termBar, t).toBeLessThan(dev);     // terminal above the devices
    }
  });

  test("Modern, wide screen: terminal left of the panel, its settings folded into the PRESET bar", async ({ page }) => {
    await page.setViewportSize({ width: 1600, height: 1000 });
    await boot(page, { params: "theme=modern&preset=cassette" });
    const panel = (await page.locator("#altair").boundingBox())!;
    const term = (await page.locator(".ws-terminal").boundingBox())!;
    expect(term.x + term.width).toBeLessThanOrEqual(panel.x + 1);   // fully to the left
    expect(term.y).toBeLessThan(panel.y + panel.height);            // ...and on the same row
    expect(panel.y).toBeLessThan(term.y + term.height);
    await expect(page.locator(".toolbars .term-bar")).toHaveCount(1);      // moved up next to PRESET
    await expect(page.locator(".ws-terminal .term-bar")).toHaveCount(0);
  });

  test("footer link list is on every theme; Mid-1990s Web dresses it Mosaic-style", async ({ page }) => {
    await boot(page, { params: "theme=web94" });
    const links = page.locator(".footer-links");
    await expect(links).toContainText("Home");
    await expect(links).toContainText("BASIC Manual");
    await expect(links.getByRole("link", { name: "Source" }))
      .toHaveAttribute("href", /github\.com\/gsirhc\/retro\/tree\/main\/retroweb\/altair8800/);
    // web94: raw underlined blue links, "Page last modified"
    await expect(links.locator("a").first()).toHaveCSS("text-decoration-line", "underline");
    await expect(links.locator("a").first()).toHaveCSS("color", "rgb(0, 0, 255)");
    await expect(page.locator(".footer")).toContainText(/Page last modified:/);

    // Win95: same links, but "Last built" and no Mosaic blue
    await page.selectOption("#pageTheme", "win");
    await expect(links).toBeVisible();
    await expect(links).toContainText("Altair Manual");
    await expect(page.locator(".footer")).toContainText(/Last built/);
    await expect(links.locator("a").first()).not.toHaveCSS("color", "rgb(0, 0, 255)");
  });

  test("Modern folds the page header into the top bar; retro keeps the big header", async ({ page }) => {
    await boot(page, { params: "theme=modern" });
    await expect(page.locator(".pagebar .pb-name")).toHaveText("ALTAIR 8800");
    expect((await page.locator(".inner > h1").boundingBox())!.height).toBeLessThan(4); // h1 kept for a11y, collapsed
    await expect(page.locator(".inner .tagline.modern-only")).not.toBeVisible();
    await expect(page.locator(".spec")).toHaveCount(0);                   // description removed

    await page.selectOption("#pageTheme", "win");
    expect((await page.locator(".inner > h1").boundingBox())!.height).toBeGreaterThan(20); // big header back
    await expect(page.locator(".pagebar .pb-name")).not.toBeVisible();
  });

  test("'Load it yourself' rides in the PRESET bar; the S-100 cards are last on the page", async ({ page }) => {
    await boot(page, { params: "preset=cassette" });
    await expect(page.locator(".preset-bar #pgToggle")).toBeVisible();
    await expect(page.locator(".preset-bar #pgToggle")).toContainText("Load it yourself");
    const dev = (await page.locator("#ptr").boundingBox())!;
    const bp = (await page.locator("#backplane").boundingBox())!;
    const foot = (await page.locator(".footer").boundingBox())!;
    expect(bp.y).toBeGreaterThan(dev.y);   // below the devices
    expect(bp.y).toBeLessThan(foot.y);     // ...just above the footer
  });

  test("the terminal settings bar sits above the monitor in every other layout", async ({ page }) => {
    await page.setViewportSize({ width: 1200, height: 900 });
    for (const t of ["win", "web94", "modern"]) {
      await boot(page, { params: `theme=${t}` });
      await expect(page.locator(".ws-terminal .term-bar"), t).toHaveCount(1);
      await expect(page.locator(".toolbars .term-bar"), t).toHaveCount(0);
    }
  });

  test("a stored theme is restored on the next visit", async ({ page }) => {
    await boot(page);
    await page.selectOption("#pageTheme", "modern");
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine);
    await expect(page.locator("html")).toHaveAttribute("data-theme", "modern");
  });

  test("the register bar shows A / BC / DE / HL / PC / SP and a clock rate", async ({ page }) => {
    await boot(page);
    await expect
      .poll(() => page.locator("#regs").innerText())
      .toMatch(/A [0-9A-F]{2}\s+BC [0-9A-F]{4}\s+DE [0-9A-F]{4}\s+HL [0-9A-F]{4}\s+PC [0-9A-F]{4}\s+SP [0-9A-F]{4}/);
    await expect.poll(() => page.locator("#regs").innerText()).toMatch(/\d\.\d MHz/);
  });

  test("the register bar reads STOP after a panel STOP", async ({ page }) => {
    await boot(page);
    await page.evaluate(() => (window as any).__test.setRunning(false));
    await expect.poll(() => page.locator("#regs").innerText()).toMatch(/STOP/);
  });

  test("?help prints the manual to the terminal", async ({ page }) => {
    await boot(page, { params: "help" });
    await waitForScreen(page, /front panel|ALTAIR|terminal/i, 15_000);
  });

  test("a keypress skips to the end of the slow ?help printout", async ({ page }) => {
    await boot(page, { params: "help&term=tty33" }); // metered at 110 baud
    await page.waitForTimeout(500);
    await page.evaluate(() => (window as any).__test.term.focus());
    await page.keyboard.press("Space");
    // the whole manual lands at once instead of trickling for ~30 s
    await waitForScreen(page, /RESET|CLR|SINGLE STEP/i, 5_000);
  });

  test("the build date populates from the wasm's Last-Modified", async ({ page }) => {
    await boot(page);
    await expect.poll(() => page.locator("#buildDate").innerText()).not.toBe("");
    await expect(page.locator("#buildDate")).not.toHaveText(/unknown/);
  });

  test("load devices persist across a reload in a custom build", async ({ page }) => {
    await boot(page);
    await page.selectOption("#preset", "");
    await expect(page.locator("#deviceChips")).toBeVisible();
    await page.check('#deviceChips input[data-dev="disk"]');
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine);
    await expect(page.locator('#deviceChips input[data-dev="disk"]')).toBeChecked();
    await expect(page.locator("#dcdd")).not.toHaveClass(/empty/);
  });
});
