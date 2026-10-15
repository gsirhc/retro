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
  await expect
    .poll(() => page.evaluate(() => (window as any).__test.machine.totalCycles()))
    .toBeGreaterThan(before);
});
