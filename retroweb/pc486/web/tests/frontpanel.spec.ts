import { test, expect } from "./fixtures";
import { boot } from "./helpers";

// The front panel's static jewelry: the fixed "66" seven-segment display
// and the Intel-homage CPU badge. Both are genuinely fixed -- no dynamic
// logic drives them (see index.html's own comments) -- so these tests
// assert the markup itself, not any behavior. Power/reset LED behavior is
// covered in boot.spec.ts.

test.describe("front panel jewelry", () => {
  test("the seven-segment display shows a fixed '66', independent of power", async ({ page }) => {
    await boot(page);
    const digits = page.locator(".sevenseg");
    await expect(digits).toHaveCount(2);
    // Digit "6" lights every segment except b (top-right) -- see index.html's
    // segment-geometry comment.
    for (let i = 0; i < 2; i++) {
      const digit = digits.nth(i);
      for (const seg of ["a", "c", "d", "e", "f", "g"]) {
        await expect(digit.locator(`.${seg}`)).toHaveClass(/on/);
      }
      await expect(digit.locator(".b")).not.toHaveClass(/on/);
    }

    // Still lit, unchanged, after powering off -- it's wired to nothing,
    // matching a real turbo-button-era case's fixed jumper-set display.
    await page.locator("#powerSwitch").click({ force: true });
    for (let i = 0; i < 2; i++) {
      await expect(digits.nth(i).locator(".a")).toHaveClass(/on/);
    }
  });

  test("the CPU badge identifies the real part, not a marketing name", async ({ page }) => {
    await boot(page);
    // The genuine Intel retail part for 66MHz was the clock-doubled DX2,
    // not a plain (never-sold) "486DX-66" -- see PC486_REVIEW.md.
    await expect(page.locator(".cpu-badge-text")).toContainText(/80486DX2/);
    await expect(page.locator(".cpu-badge-text")).toContainText(/66 MHz/);
  });
});
