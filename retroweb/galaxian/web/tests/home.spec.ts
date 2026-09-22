import { test, expect } from "@playwright/test";

const HOME = "http://localhost:8810/";

test.describe("retroweb landing page", () => {
  test("lists Galaxian Arcade under an Arcade section, linking to /galaxian/", async ({ page }) => {
    await page.goto(HOME);
    await expect(page).toHaveTitle(/RETRO/i);

    const card = page.locator('a.machine-card[href="galaxian/"]');
    await expect(card.locator(".name")).toHaveText(/Galaxian Arcade/i);
    const headings = page.locator("h2.section-heading");
    await expect(headings).toHaveText(["Arcade: Z-80 Powered", "Homebrew"]);

    const shot = card.locator("img.shot");
    await expect(shot).toHaveAttribute("src", /assets\/galaxian-cabinet\.png$/);
    await expect(shot).toHaveAttribute("width", "286");
    await expect(shot).toHaveAttribute("height", "128");
    await expect(shot).toHaveJSProperty("complete", true);
    expect(await shot.evaluate((img: HTMLImageElement) => img.naturalWidth)).toBe(286);
    expect(await shot.evaluate((img: HTMLImageElement) => img.naturalHeight)).toBe(128);
  });

  test("arcade cards are alphabetical; computers stay Altair then IBM", async ({ page }) => {
    await page.goto(HOME);
    const computers = await page.locator(".machines").first().locator(".name").allTextContents();
    expect(computers).toEqual(["MITS Altair 8800", "IBM PC/AT"]);
    const arcade = await page.locator('h2.section-heading:has-text("Arcade") + .machines .name').allTextContents();
    expect(arcade).toEqual([
      "Frogger Arcade",
      "Galaxian Arcade",
      "Ms. Pac-Man Arcade",
      "Pac-Man Arcade",
      "Scramble Arcade",
    ]);
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
    await expect(footer).toContainText(/\(c\) 2026 RetroCG/);
    await expect(footer).toContainText(/old-ass tech/);
    const about = footer.locator("a.about-sign");
    await expect(about).toHaveAttribute("href", "about.html");
    await expect(about.locator(".about-sign-win")).toBeVisible();
    await about.click();
    await expect(page).toHaveURL(/\/about\.html$/);
    await expect(page).toHaveTitle(/About/i);
    await expect(page.locator("a.pb-close")).toHaveAttribute("href", "./");
    await page.locator("a.pb-close").click();
    await expect(page).toHaveTitle(/RETRO — vintage machines/i);
  });
});
