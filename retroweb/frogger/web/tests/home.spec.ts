import { test, expect } from "@playwright/test";

const HOME = "http://localhost:8610/";

test.describe("retroweb landing page", () => {
  test("lists Frogger Arcade under an Arcade section, linking to /frogger/", async ({ page }) => {
    await page.goto(HOME);
    await expect(page).toHaveTitle(/Retro Computers & Games/);

    const card = page.locator('a.machine-card[href="frogger/"]');
    await expect(card.locator(".name")).toHaveText(/Frogger Arcade/i);
    const headings = page.locator("h2.section-heading");
    await expect(headings).toHaveText(["Arcade: Z-80 Powered", "Homebrew"]);

    const shot = card.locator("img.shot");
    await expect(shot).toHaveAttribute("src", /assets\/frogger-cabinet\.png$/);
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
    await expect(page.locator("#siteFooter .about-sign-win")).toBeVisible();

    await page.selectOption("#pageTheme", "modern");
    await expect(root).toHaveAttribute("data-theme", "modern");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe("modern");
    await expect(page.locator("#siteFooter .about-sign-about")).toBeVisible();
  });

  test("the shared site footer credits RetroCG and the About sign goes to about.html", async ({ page }) => {
    await page.goto(HOME);
    const footer = page.locator("#siteFooter");
    await expect(footer).toContainText(/© 2026 RetroCG/);
    await expect(footer).toContainText(/old-ass tech/);
    const about = footer.locator("a.about-sign");
    await expect(about).toHaveAttribute("href", "about.html");
    await expect(about.locator(".about-sign-win")).toBeVisible();
    await about.click();
    await expect(page).toHaveURL(/\/about\.html$/);
    await expect(page).toHaveTitle(/About/i);
    await expect(page.locator("a.pb-close")).toHaveAttribute("href", "./");
    await page.locator("a.pb-close").click();
    await expect(page).toHaveTitle(/Retro Computers & Games/);
  });
});
