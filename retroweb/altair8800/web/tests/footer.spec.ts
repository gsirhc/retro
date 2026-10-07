import { test, expect } from "@playwright/test";

// Shared site footer (shared/footer.js); Help points one level up at about.html.

test("site footer is present and the Help control points at ../about.html", async ({ page }) => {
  await page.goto("/?test=1");
  const footer = page.locator("#siteFooter");
  await expect(footer).toContainText(/© 2026 RetroCG/);
  await expect(footer).toContainText(/old-ass computer/);
  await expect(footer).toContainText(/Source code available on GitHub/);
  await expect(footer.locator("a.source-link")).toHaveText("GitHub");
  await expect(footer.locator("a.source-link")).toHaveAttribute(
    "href",
    "https://github.com/gsirhc/retro/tree/main/retroweb",
  );
  const help = footer.locator("a.about-sign");
  await expect(help).toHaveAttribute("href", "../about.html");
  await expect(help).toHaveText("Help");
});
