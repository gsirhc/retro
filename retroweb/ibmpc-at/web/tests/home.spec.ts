import { test, expect } from "@playwright/test";


async function chooseTheme(page, theme, mode = "light") {
  await page.locator("#pageThemeBtn").click();
  await page.locator(`#themeDialog input[name="pageThemeFamily"][value="${theme}"]`).check();
  await page.locator(`#themeDialog input[name="pageThemeMode"][value="${mode}"]`).check();
  await page.locator("#themeDialogDone").click();
}


// Landing page on :8310. Shares the retro8080.theme key with the emulator.

const HOME = "http://localhost:8310/";

test.describe("retroweb landing page", () => {
  test("lists the IBM PC/AT with a launch link to /ibmpc-at/", async ({ page }) => {
    await page.goto(HOME);
    await expect(page).toHaveTitle(/Retro Computers & Games/);

    const card = page.locator('a.machine-card[href="ibmpc-at/"]');
    await expect(card).toHaveAttribute("href", "ibmpc-at/");
    await expect(card.locator(".name")).toHaveText(/IBM PC\/AT/i);

    const shot = card.locator("img.shot");
    await expect(shot).toHaveAttribute("src", /assets\/ibmpcat-panel\.jpg$/);
    await expect(shot).toHaveAttribute("width", "712");
    await expect(shot).toHaveAttribute("height", "128");
    await expect(shot).toHaveJSProperty("complete", true);
    expect(await shot.evaluate((img: HTMLImageElement) => img.naturalWidth)).toBe(712);
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

    await chooseTheme(page, "web94");
    await expect(root).toHaveAttribute("data-theme", "web94");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe("web94");
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

  test("a stored retro8080.theme is honoured on load", async ({ page }) => {
    await page.goto(HOME);
    await page.evaluate(() => localStorage.setItem("retro8080.theme", "modern"));
    await page.reload();
    await expect(page.locator("html")).toHaveAttribute("data-theme", "modern");
    await expect(page.locator("#pageThemeBtn")).toHaveText("Modern");
  });
});
