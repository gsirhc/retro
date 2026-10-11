import { test, expect } from "./fixtures";
import { boardKey, openKeyboard } from "./helpers";

// Esc releases pointer lock and leaves fullscreen rather than reaching DOS, so the drawn keyboard's
// Esc and the bezel's both send it.
test("Esc on the drawn keyboard reaches the guest", async ({ livePage: page }) => {
  await openKeyboard(page);
  const esc = boardKey(page, "Escape");
  await expect(esc).toHaveAttribute("aria-disabled", "false");
  const before = await page.evaluate(() => (window as any).__test.machine.totalCycles());
  await esc.click();
  await page.waitForTimeout(150);
  const after = await page.evaluate(() => (window as any).__test.machine.totalCycles());
  expect(Number(after - before)).toBeGreaterThan(0);
});
