import { test, expect } from "./fixtures";

// The front panel's static jewelry: fixed green "66" seven-segment display
// and the Turbo LED (cosmetic only -- never changes the guest clock).
// Power/reset behavior is covered in boot.spec.ts.

test.describe("front panel jewelry", () => {
  test("the seven-segment display shows a fixed '66', independent of power", async ({ livePage: page }) => {
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

  test("reads as a tower turbo cluster with 5.25\" CD above 3.5\" floppy", async ({ livePage: page }) => {
    await expect(page.locator(".tower-panel")).toBeVisible();
    await expect(page.locator("#turboBtn")).toBeVisible();
    await expect(page.locator("#resetBtn")).toBeVisible();
    await expect(page.locator("#powerSwitch")).toBeVisible();
    await expect(page.locator(".power-rocker")).toBeVisible();
    await expect(page.locator(".tower-keylock")).toHaveCount(0);

    // Usual tower stack: 5.25" CD-ROM on top, 3.5" floppy below.
    const drives = page.locator(".at-drives .at-bay");
    await expect(drives).toHaveCount(2);
    await expect(drives.nth(0)).toHaveAttribute("data-drive", "cdrom");
    await expect(drives.nth(0)).toHaveClass(/bay-525/);
    await expect(drives.nth(0).locator(".cd-door")).toBeVisible();
    await expect(drives.nth(1)).toHaveAttribute("data-drive", "0");
    await expect(drives.nth(1)).toHaveClass(/bay-35/);
    await expect(drives.nth(1).locator(".floppy-door")).toBeVisible();
  });

  test("Turbo toggles its amber LED only -- never the guest clock", async ({ livePage: page }) => {
    const btn = page.locator("#turboBtn");
    const led = page.locator("#turboLed");
    await expect(btn).toHaveAttribute("aria-pressed", "true");
    await expect(led).toHaveClass(/turbo-on/);

    const cycles1 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    await btn.click();
    await expect(btn).toHaveAttribute("aria-pressed", "false");
    await expect(led).not.toHaveClass(/turbo-on/);
    await page.waitForTimeout(200);
    const cycles2 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    // Still advancing at real speed -- Turbo is jewelry, not a clock multiplier.
    expect(cycles2).toBeGreaterThan(cycles1);

    await btn.click();
    await expect(btn).toHaveAttribute("aria-pressed", "true");
    await expect(led).toHaveClass(/turbo-on/);
  });
});
