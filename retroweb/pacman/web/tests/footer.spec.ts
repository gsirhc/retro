import { test, expect } from "@playwright/test";

// Shared site footer (shared/footer.js). Injected on every machine page;
// the EXIT sign points one level up at about.html on the staged site.

test("site footer is present and the EXIT sign points at ../about.html", async ({ page }) => {
  await page.goto("/?test=1");
  const footer = page.locator("#siteFooter");
  await expect(footer).toContainText(/Copyright 2026/);
  await expect(footer).toContainText(/machine intelligence/i);
  await expect(footer.locator("a.exit-sign")).toHaveAttribute("href", "../about.html");
});
