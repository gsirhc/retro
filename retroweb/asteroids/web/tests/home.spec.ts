import { test, expect } from "@playwright/test";

// The retroweb/ landing page (served on :9010 by the 2nd webServer in
// playwright.config.ts). It shares the retro8080.theme localStorage key
// with the emulator, so a theme picked here carries into /asteroids/ and
// back. Needs the Asteroids landing-page card wired into retroweb/index.html
// -- see "Wiring a machine into the site" in CLAUDE.md; that is a separate
// step from this machine's own web/ front end.

const HOME = "http://localhost:9010/";

test.describe("retroweb landing page", () => {
  test("lists Asteroids Arcade under an Arcade section, linking to /asteroids/", async ({ page }) => {
    await page.goto(HOME);
    await expect(page).toHaveTitle(/Retro Computers & Games/);

    const card = page.locator('a.machine-card[href="asteroids/"]');
    await expect(card.locator(".name")).toHaveText(/Asteroids Arcade/i);

    const shot = card.locator("img.shot");
    await expect(shot).toHaveAttribute("src", /assets\/asteroids-cabinet\.png$/);
    await expect(shot).toHaveAttribute("width", "286");
    await expect(shot).toHaveAttribute("height", "128");
    await expect(shot).toHaveJSProperty("complete", true);
    expect(await shot.evaluate((img: HTMLImageElement) => img.naturalWidth)).toBe(286);
    expect(await shot.evaluate((img: HTMLImageElement) => img.naturalHeight)).toBe(128);
  });

  test("the theme selector switches the page and persists to retro8080.theme", async ({ page }) => {
    await page.goto(HOME);
    const root = page.locator("html");
    await expect(root).toHaveAttribute("data-theme", "win");
    await expect(page.locator("#siteFooter a.about-sign")).toHaveText("Help");

    await page.selectOption("#pageTheme", "modern");
    await expect(root).toHaveAttribute("data-theme", "modern");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe("modern");
    await expect(page.locator("#siteFooter a.about-sign")).toHaveText("Help");
  });

  test("the shared site footer credits RetroCG and the Help control goes to about.html", async ({ page }) => {
    await page.goto(HOME);
    const footer = page.locator("#siteFooter");
    await expect(footer).toContainText(/© 2026 RetroCG/);
    await expect(footer).toContainText(/old-ass computer/);
    const about = footer.locator("a.about-sign");
    await expect(about).toHaveAttribute("href", "about.html");
    await expect(about).toHaveText("Help");
    await about.click();
    await expect(page).toHaveURL(/\/about\.html$/);
    await expect(page).toHaveTitle(/About/i);
    await expect(page.locator("a.pb-close")).toHaveAttribute("href", "./");
    await page.locator("a.pb-close").click();
    await expect(page).toHaveTitle(/Retro Computers & Games/);
  });
});
