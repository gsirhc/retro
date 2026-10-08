import { test, expect } from "@playwright/test";


async function chooseTheme(page, theme, mode = "light") {
  await page.locator("#pageThemeBtn").click();
  await page.locator(`#themeDialog input[name="pageThemeFamily"][value="${theme}"]`).check();
  await page.locator(`#themeDialog input[name="pageThemeMode"][value="${mode}"]`).check();
  await page.locator("#themeDialogDone").click();
}


const HOME = "http://localhost:8610/";

test.describe("retroweb landing page", () => {
  test("lists Frogger Arcade under an Arcade section, linking to /frogger/", async ({ page }) => {
    await page.goto(HOME);
    await expect(page).toHaveTitle(/Retro Computers & Games/);

    const card = page.locator('a.machine-card[href="frogger/"]');
    await expect(card.locator(".name")).toHaveText(/Frogger Arcade/i);
    const headings = page.locator("h2.section-heading");
    await expect(headings).toHaveText(["Arcade: Z-80 Powered", "Arcade: 6502 Powered", "Homebrew"]);

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

  test("Windows title-bar buttons: soft-minimize collapses the page; close is disabled", async ({ page }) => {
    await page.goto(HOME);
    await expect(page.locator(".pb-close.is-disabled")).toHaveAttribute("aria-disabled", "true");
    await expect(page.locator("a.pb-close")).toHaveCount(0);
    await expect(page.locator(".pb-max")).toBeDisabled();
    await expect(page.locator(".inner")).toBeVisible();
    await page.locator(".pb-min").click();
    await expect(page.locator(".page")).toHaveClass(/is-minimized/);
    await expect(page.locator(".inner")).toBeHidden();
    await expect(page.locator("#siteFooter")).toBeHidden();
    await page.locator(".pb-min").click();
    await expect(page.locator(".page")).not.toHaveClass(/is-minimized/);
    await expect(page.locator(".inner")).toBeVisible();
  });

});
