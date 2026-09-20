import { test, expect } from "@playwright/test";

// The retroweb/ landing page (served on :8510 by the 2nd webServer in
// playwright.config.ts). It shares the retro8080.theme localStorage key
// with the emulator, so a theme picked here carries into /pacman/ and back.

const HOME = "http://localhost:8510/";

test.describe("retroweb landing page", () => {
  test("lists Pac-Man Arcade under an Arcade section, linking to /pacman/", async ({ page }) => {
    await page.goto(HOME);
    await expect(page).toHaveTitle(/RETRO/i);

    const card = page.locator('a.machine-card[href="pacman/"]');
    await expect(card.locator(".name")).toHaveText(/Pac-Man Arcade/i);

    const shot = card.locator("img.shot");
    await expect(shot).toHaveAttribute("src", /assets\/pacman-cabinet\.png$/);
    await expect(shot).toHaveJSProperty("complete", true);
    expect(
      await shot.evaluate((img: HTMLImageElement) => img.naturalWidth),
    ).toBeGreaterThan(0);
  });

  test("lists Ms. Pac-Man Arcade on the same page via ?game=mspacman", async ({ page }) => {
    await page.goto(HOME);
    const card = page.locator('a.machine-card[href="pacman/?game=mspacman"]');
    await expect(card.locator(".name")).toHaveText(/Ms\. Pac-Man Arcade/i);
    await expect(card.locator("img.shot")).toHaveAttribute("src", /assets\/mspacman-cabinet\.png$/);
    await expect(card.locator("img.shot")).toHaveJSProperty("complete", true);
    expect(
      await card.locator("img.shot").evaluate((img: HTMLImageElement) => img.naturalWidth),
    ).toBeGreaterThan(0);
  });

  test("the theme selector switches the page and persists to retro8080.theme", async ({ page }) => {
    await page.goto(HOME);
    const root = page.locator("html");
    await expect(root).toHaveAttribute("data-theme", "win");

    await page.selectOption("#pageTheme", "modern");
    await expect(root).toHaveAttribute("data-theme", "modern");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe("modern");
  });

  test("the shared site footer credits Cursor/Claude and the EXIT sign goes to about.html", async ({ page }) => {
    await page.goto(HOME);
    const footer = page.locator("#siteFooter");
    await expect(footer).toContainText(/Copyright 2026/);
    await expect(footer).toContainText(/machine intelligence/i);
    const exit = footer.locator("a.exit-sign");
    await expect(exit).toHaveAttribute("href", "about.html");
    await exit.click();
    await expect(page).toHaveURL(/\/about\.html$/);
    await expect(page).toHaveTitle(/About/i);
  });
});
