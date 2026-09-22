import { test, expect } from "@playwright/test";

// Shared site footer (shared/footer.js). Injected on every machine page;
// the About sign points one level up at about.html on the staged site.

test("site footer is present and the About sign points at ../about.html", async ({ page }) => {
  await page.goto("/");
  const footer = page.locator("#siteFooter");
  await expect(footer).toContainText(/\(c\) 2026 RetroCG/);
  await expect(footer).toContainText(/old-a\$\$ tech/);
  await expect(footer).toContainText(/Source code available on GitHub/);
  await expect(footer.locator("a.source-link")).toHaveText("GitHub");
  await expect(footer.locator("a.source-link")).toHaveAttribute(
    "href",
    "https://github.com/gsirhc/retro/tree/main/retroweb",
  );
  await expect(footer.locator("a.about-sign")).toHaveAttribute("href", "../about.html");
  await expect(footer.locator(".about-sign-win")).toBeVisible();
  await expect(footer.locator(".about-sign-about")).toBeHidden();
});
