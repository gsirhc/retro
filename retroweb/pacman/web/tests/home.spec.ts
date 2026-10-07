import { test, expect } from "@playwright/test";


async function chooseTheme(page, theme, mode = "light") {
  await page.locator("#pageThemeBtn").click();
  await page.locator(`#themeDialog input[name="pageThemeFamily"][value="${theme}"]`).check();
  await page.locator(`#themeDialog input[name="pageThemeMode"][value="${mode}"]`).check();
  await page.locator("#themeDialogDone").click();
}


// The retroweb/ landing page (served on :8510 by the 2nd webServer in
// playwright.config.ts). It shares the retro8080.theme localStorage key
// with the emulator, so a theme picked here carries into /pacman/ and back.

const HOME = "http://localhost:8510/";

test.describe("retroweb landing page", () => {
  test("lists Pac-Man Arcade under an Arcade section, linking to /pacman/", async ({ page }) => {
    await page.goto(HOME);
    await expect(page).toHaveTitle(/Retro Computers & Games/);
    await expect(page.locator("h2.section-heading")).toHaveText([
      "Arcade: Z-80 Powered",
      "Homebrew",
    ]);

    const card = page.locator('a.machine-card[href="pacman/"]');
    await expect(card.locator(".name")).toHaveText(/Pac-Man Arcade/i);

    const shot = card.locator("img.shot");
    await expect(shot).toHaveAttribute("src", /assets\/pacman-cabinet\.png$/);
    await expect(shot).toHaveAttribute("width", "286");
    await expect(shot).toHaveAttribute("height", "128");
    await expect(shot).toHaveJSProperty("complete", true);
    expect(await shot.evaluate((img: HTMLImageElement) => img.naturalWidth)).toBe(286);
    expect(await shot.evaluate((img: HTMLImageElement) => img.naturalHeight)).toBe(128);
  });

  test("lists Ms. Pac-Man Arcade on the same page via ?game=mspacman", async ({ page }) => {
    await page.goto(HOME);
    const card = page.locator('a.machine-card[href="pacman/?game=mspacman"]');
    await expect(card.locator(".name")).toHaveText(/Ms\. Pac-Man Arcade/i);
    const shot = card.locator("img.shot");
    await expect(shot).toHaveAttribute("src", /assets\/mspacman-cabinet\.png$/);
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

    await chooseTheme(page, "modern");
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
