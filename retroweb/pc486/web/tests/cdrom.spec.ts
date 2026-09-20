import { test, expect } from "./fixtures";
import { boot, bay, insertCdrom, ejectCdrom } from "./helpers";
import * as fs from "fs";
import * as os from "os";
import * as path from "path";

function makeBlankIso(bytes: number): string {
  const p = path.join(os.tmpdir(), `pc486-test-${Date.now()}-${Math.random().toString(36).slice(2)}.iso`);
  fs.writeFileSync(p, Buffer.alloc(bytes, 0));
  return p;
}

test.describe("CD-ROM drive", () => {
  test("ships with the FreeDOS install/live CD already in the drive", async ({ page }) => {
    await boot(page);

    const bayCd = bay(page, "cdrom");
    await expect(bayCd).toHaveClass(/loaded/);
    const label = bayCd.locator('[data-role="label"]');
    await expect(label).not.toHaveClass(/empty/);
    await expect(bayCd.locator('[data-role="eject"]')).toBeEnabled();

    const isPresent = await page.evaluate(() => (window as any).__test.machine.cdromPresent());
    expect(isPresent).toBe(true);
  });

  test("ejecting empties the bay; inserting a new ISO loads it", async ({ page }) => {
    await boot(page);

    await ejectCdrom(page);
    const bayCd = bay(page, "cdrom");
    await expect(bayCd).not.toHaveClass(/loaded/);
    const label = bayCd.locator('[data-role="label"]');
    await expect(label).toHaveClass(/empty/);
    await expect(bayCd.locator('[data-role="eject"]')).toBeDisabled();
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.machine.cdromPresent()))
      .toBe(false);

    const isoPath = makeBlankIso(2048 * 16);  // a handful of 2048-byte CD-ROM sectors
    await insertCdrom(page, isoPath);
    await expect(bayCd).toHaveClass(/loaded/);
    await expect(label).toContainText(path.basename(isoPath));
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.machine.cdromPresent()))
      .toBe(true);
  });

  test("swapping discs is independent of power state -- works while the machine is off", async ({ page }) => {
    // A real CD-ROM tray opens/closes with or without power, just like the
    // floppy bay -- see app.js's pendingCdrom.
    await boot(page);

    await page.locator("#powerSwitch").click({ force: true });
    await expect(page.locator("#powerLed")).not.toHaveClass(/power-on/);

    await ejectCdrom(page);
    const bayCd = bay(page, "cdrom");
    await expect(bayCd).not.toHaveClass(/loaded/);

    const isoPath = makeBlankIso(2048 * 16);
    await insertCdrom(page, isoPath);
    await expect(bayCd).toHaveClass(/loaded/);
  });
});
