import { test, expect } from "@playwright/test";

// The retroweb/ landing page (served on :8110 by the 2nd webServer in
// playwright.config.ts). It shares the `retro8080.theme` localStorage key with
// the emulator, so a theme picked here carries into /altair8800/ and back.

const HOME = "http://localhost:8110/";

test.describe("retroweb landing page", () => {
  test("lists the Altair 8800 with a launch link to /altair8800/", async ({ page }) => {
    await page.goto(HOME);
    await expect(page).toHaveTitle(/RETRO/i);

    // scoped by href -- assembler6502/ and ibmpc-at/ each have their own
    // .machine-card on the same page now too
    const card = page.locator('a.machine-card[href="altair8800/"]');
    await expect(card).toHaveAttribute("href", "altair8800/"); // resolves in the deployed _site
    await expect(card.locator(".name")).toHaveText(/MITS Altair 8800/i);

    // the front-panel thumbnail is served from assets/ and decodes
    const shot = card.locator("img.shot");
    await expect(shot).toHaveAttribute("src", /assets\/altair-panel\.jpg$/);
    await expect(shot).toHaveAttribute("width", "286");
    await expect(shot).toHaveAttribute("height", "128");
    await expect(shot).toHaveJSProperty("complete", true);
    expect(await shot.evaluate((img: HTMLImageElement) => img.naturalWidth)).toBe(286);
    expect(await shot.evaluate((img: HTMLImageElement) => img.naturalHeight)).toBe(128);
  });

  test("the theme selector switches the page and persists to retro8080.theme", async ({ page }) => {
    await page.goto(HOME);
    const root = page.locator("html");
    await expect(root).toHaveAttribute("data-theme", "win"); // default

    await expect(page.locator("#siteFooter .about-sign-win")).toBeVisible();

    await page.selectOption("#pageTheme", "modern");
    await expect(root).toHaveAttribute("data-theme", "modern");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe("modern");
    await expect(page.locator("#siteFooter .about-sign-about")).toBeVisible();

    await page.selectOption("#pageTheme", "web94");
    await expect(root).toHaveAttribute("data-theme", "web94");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe("web94");
    await expect(page.locator("#siteFooter .about-sign-about")).toBeVisible();
  });

  test("a stored retro8080.theme is honoured on load", async ({ page }) => {
    await page.goto(HOME);
    await page.evaluate(() => localStorage.setItem("retro8080.theme", "modern"));
    await page.reload();
    await expect(page.locator("html")).toHaveAttribute("data-theme", "modern");
    await expect(page.locator("#pageTheme")).toHaveValue("modern");
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
