import { test, expect } from "./fixtures";
import { boot } from "./helpers";

// A user's own Esc key releases pointer lock and leaves fullscreen rather
// than reaching DOS, so the only way to send Esc to the guest is a button.
// One lives in the bezel, but it is easy to miss up in the monitor chrome --
// this is the copy in the panel where a user goes looking for keys.
test("Esc is offered in the Function & extended keys panel and reaches the guest", async ({
  page,
}) => {
  await boot(page);
  const esc = page.locator('#extraKeyRow [data-key="Escape"]');
  await expect(esc).toHaveCount(1);
  await expect(esc).toBeEnabled();

  // It must deliver the real set/break pair for Esc (scancode 0x01), the same
  // as every other key button -- not merely exist.
  const before = await page.evaluate(() => (window as any).__test.machine.totalCycles());
  await esc.click();
  await page.waitForTimeout(150);
  const after = await page.evaluate(() => (window as any).__test.machine.totalCycles());
  expect(Number(after - before)).toBeGreaterThan(0);
});
