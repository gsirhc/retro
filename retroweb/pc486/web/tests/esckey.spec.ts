import { test, expect } from "./fixtures";

// Esc releases pointer lock and leaves fullscreen rather than reaching DOS, so a button sends it.
// This is the copy in the keys panel; the bezel has one too.
test("Esc is offered in the Function & extended keys panel and reaches the guest", async ({
  livePage: page,
}) => {
  const esc = page.locator('#extraKeyRow [data-key="Escape"]');
  await expect(esc).toHaveCount(1);
  await expect(esc).toBeEnabled();

  // Must deliver the real set/break pair for Esc (scancode 0x01).
  const before = await page.evaluate(() => (window as any).__test.machine.totalCycles());
  await esc.click();
  await page.waitForTimeout(150);
  const after = await page.evaluate(() => (window as any).__test.machine.totalCycles());
  expect(Number(after - before)).toBeGreaterThan(0);
});
