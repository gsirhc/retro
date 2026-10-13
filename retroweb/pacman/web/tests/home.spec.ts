import { test, expect } from "@playwright/test";


async function chooseTheme(page, theme, mode = "light") {
  await page.locator("#pageThemeBtn").click();
  await page.locator(`#themeDialog input[name="pageThemeFamily"][value="${theme}"]`).check();
  await page.locator(`#themeDialog input[name="pageThemeMode"][value="${mode}"]`).check();
  await page.locator("#themeDialogDone").click();
}


// Landing page on :8510. Shares the retro8080.theme key with the emulator.

const HOME = "http://localhost:8510/";
const ARCADE = "http://localhost:8510/arcade/";

test.describe("retroweb landing page", () => {
  test("Arcade section is a full-width Enter the Arcade portal; cabinets stay on the hub", async ({ page }) => {
    await page.goto(HOME);
    await expect(page).toHaveTitle(/Retro In-browser Systems & Games/);
    await expect(page.locator("h2.section-heading")).toHaveText([
      "Systems",
      "Ready to Play",
      "Arcade",
      "Homebrew",
    ]);

    await expect(page.locator('a.machine-card[href="pacman/"]')).toHaveCount(0);
    await expect(page.locator('a.machine-card[href="galaga/"]')).toHaveCount(0);
    await expect(page.locator('a.machine-card[href="frogger/"]')).toHaveCount(0);

    const portal = page.locator("a.arcade-portal");
    await expect(portal).toHaveAttribute("href", "arcade/");
    await expect(portal).toHaveAttribute("aria-label", "Enter the Arcade");
    await expect(portal.locator(".portal-neon")).toHaveText("Enter the Arcade");
    await portal.click();
    await expect(page).toHaveURL(/\/arcade\/?$/);
    await expect(page).toHaveTitle(/Arcade/);
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

  test("the shared site footer credits RISG and the Help control goes to about.html", async ({ page }) => {
    await page.goto(HOME);
    const footer = page.locator("#siteFooter");
    await expect(footer).toContainText(/© 2026 RISG/);
    await expect(footer).toContainText(/old-ass computer/);
    const about = footer.locator("a.about-sign");
    await expect(about).toHaveAttribute("href", "about.html");
    await expect(about).toHaveText("Help");
    await about.click();
    await expect(page).toHaveURL(/\/about\.html$/);
    await expect(page).toHaveTitle(/About/i);
    await expect(page.locator("a.pb-close")).toHaveAttribute("href", "./");
    await page.locator("a.pb-close").click();
    await expect(page).toHaveTitle(/Retro In-browser Systems & Games/);
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

test.describe("arcade hub", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto(ARCADE);
    await page.evaluate(() => localStorage.removeItem("retroweb.arcadeSort"));
    await page.reload();
  });

  test("lists eight cabinets in Name order by default, including Pac-Man and Ms. Pac-Man", async ({ page }) => {
    await expect(page).toHaveTitle(/Arcade/);
    await expect(page.locator("#arcadeCatalog > h2.section-heading")).toHaveCount(0);
    const names = await page.locator("#arcadeCatalog a.machine-card .name").allTextContents();
    expect(names).toEqual([
      "Asteroids Arcade",
      "Centipede Arcade",
      "Frogger Arcade",
      "Galaga Arcade",
      "Galaxian Arcade",
      "Ms. Pac-Man Arcade",
      "Pac-Man Arcade",
      "Scramble Arcade",
    ]);
    const pac = page.locator('a.machine-card[href="../pacman/"]');
    await expect(pac.locator(".name")).toHaveText(/Pac-Man Arcade/i);
    const shot = pac.locator("img.shot");
    await expect(shot).toHaveAttribute("src", /assets\/pacman-cabinet\.png$/);
    await expect(shot).toHaveJSProperty("complete", true);
    expect(await shot.evaluate((img: HTMLImageElement) => img.naturalWidth)).toBe(286);
    expect(await shot.evaluate((img: HTMLImageElement) => img.naturalHeight)).toBe(128);

    const ms = page.locator('a.machine-card[href="../pacman/?game=mspacman"]');
    await expect(ms.locator(".name")).toHaveText(/Ms\. Pac-Man Arcade/i);
  });

  test("Sort by Year / CPU / Manufacturer groups with section headings and persists", async ({ page }) => {
    const sel = page.locator("#arcadeSort");
    await expect(sel).toHaveValue("name");

    await sel.selectOption("year");
    expect(await page.evaluate(() => localStorage.getItem("retroweb.arcadeSort"))).toBe("year");
    await expect(page.locator("#arcadeCatalog > h2.section-heading")).toHaveText([
      "1979", "1980", "1981", "1982",
    ]);
    let names = await page.locator("#arcadeCatalog a.machine-card .name").allTextContents();
    expect(names).toEqual([
      "Asteroids Arcade",
      "Galaxian Arcade",
      "Pac-Man Arcade",
      "Centipede Arcade",
      "Frogger Arcade",
      "Galaga Arcade",
      "Scramble Arcade",
      "Ms. Pac-Man Arcade",
    ]);

    await sel.selectOption("cpu");
    await expect(page.locator("#arcadeCatalog > h2.section-heading")).toHaveText(["6502", "Z80"]);
    names = await page.locator("#arcadeCatalog a.machine-card .name").allTextContents();
    expect(names.slice(0, 2)).toEqual(["Asteroids Arcade", "Centipede Arcade"]);
    expect(names.slice(2)).toEqual([
      "Frogger Arcade",
      "Galaga Arcade",
      "Galaxian Arcade",
      "Ms. Pac-Man Arcade",
      "Pac-Man Arcade",
      "Scramble Arcade",
    ]);

    await sel.selectOption("manufacturer");
    await expect(page.locator("#arcadeCatalog > h2.section-heading")).toHaveText([
      "Atari", "Konami", "Midway", "Namco",
    ]);
    names = await page.locator("#arcadeCatalog a.machine-card .name").allTextContents();
    expect(names).toEqual([
      "Asteroids Arcade",
      "Centipede Arcade",
      "Frogger Arcade",
      "Scramble Arcade",
      "Galaga Arcade",
      "Ms. Pac-Man Arcade",
      "Galaxian Arcade",
      "Pac-Man Arcade",
    ]);

    await page.reload();
    await expect(page.locator("#arcadeSort")).toHaveValue("manufacturer");
    await expect(page.locator("#arcadeCatalog > h2.section-heading")).toHaveText([
      "Atari", "Konami", "Midway", "Namco",
    ]);
    names = await page.locator("#arcadeCatalog a.machine-card .name").allTextContents();
    expect(names[0]).toBe("Asteroids Arcade");
    expect(names[names.length - 1]).toBe("Pac-Man Arcade");
  });
});
