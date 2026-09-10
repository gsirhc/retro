import { test, expect } from "@playwright/test";

// Boots the real, unmodified firmware straight to Wozmon -- the browser
// analog of tests/machine_test.cpp's BootsTheRealFirmwareStraightToWozmon.

test("boots the real ROM straight to the Wozmon prompt", async ({ page }) => {
  const errors: string[] = [];
  page.on("pageerror", (e) => errors.push(e.message));

  await page.goto("/");
  await expect(page.locator("#screen")).toContainText("\\", { timeout: 25000 });
  // FullBoard's LCD carries no boot banner any more -- RESET just clears it.
  expect(await page.evaluate(() => window.__machine.lcdText())).toBe(" ".repeat(32));

  expect(errors).toEqual([]);
});

test("a real examine command round-trips through the terminal", async ({ page }) => {
  await page.goto("/");
  await expect(page.locator("#screen")).toContainText("\\", { timeout: 25000 });
  await page.click("#screen");
  // Typed with a real per-character delay: the ACIA has only a one-byte RX
  // register, so characters arriving faster than the emulator's frame loop
  // can drain them (via the NMI handler) overwrite each other -- a real
  // hardware constraint, not a test artifact. See app.js's term.onData for
  // the pacing gap this currently relies on typing speed to avoid.
  await page.keyboard.type("0.F", { delay: 100 });
  await page.keyboard.press("Enter");
  await expect(page.locator("#screen")).toContainText("0000:", { timeout: 5000 });
});
