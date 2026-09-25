import { test, expect } from "./fixtures";
import { boot, bay, insertCdrom, ejectCdrom, loadFreedosCdrom } from "./helpers";
import * as fs from "fs";
import * as os from "os";
import * as path from "path";

function makeBlankIso(bytes: number): string {
  const p = path.join(os.tmpdir(), `pc486-test-${Date.now()}-${Math.random().toString(36).slice(2)}.iso`);
  fs.writeFileSync(p, Buffer.alloc(bytes, 0));
  return p;
}

test.describe("CD-ROM drive", () => {
  test("boots with the drive empty, and never fetches the FreeDOS CD unasked", async ({ page }) => {
    const requests: string[] = [];
    page.on("request", (req) => requests.push(req.url()));

    await boot(page);

    const bayCd = bay(page, "cdrom");
    await expect(bayCd).not.toHaveClass(/loaded/);
    const label = bayCd.locator('[data-role="label"]');
    await expect(label).toHaveClass(/empty/);
    await expect(bayCd.locator('[data-role="eject"]')).toBeDisabled();

    const isPresent = await page.evaluate(() => (window as any).__test.machine.cdromPresent());
    expect(isPresent).toBe(false);
    expect(requests.some((u) => u.includes("freedos-cd.iso"))).toBe(false);
  });

  test('"Load FreeDOS CD..." fetches and mounts it on demand', async ({ page }) => {
    await boot(page);

    const bayCd = bay(page, "cdrom");
    await loadFreedosCdrom(page);
    await expect(bayCd).toHaveClass(/loaded/, { timeout: 60_000 });
    const label = bayCd.locator('[data-role="label"]');
    await expect(label).toContainText("FreeDOS install/live CD");
    await expect(bayCd.locator('[data-role="eject"]')).toBeEnabled();
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.machine.cdromPresent()))
      .toBe(true);
  });

  test("ejecting empties the bay; inserting a new ISO loads it", async ({ page }) => {
    await boot(page);
    const bayCd = bay(page, "cdrom");

    const isoPath = makeBlankIso(2048 * 16);  // a handful of 2048-byte CD-ROM sectors
    const label = bayCd.locator('[data-role="label"]');
    await insertCdrom(page, isoPath);
    await expect(bayCd).toHaveClass(/loaded/);
    await expect(label).toContainText(path.basename(isoPath));
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.machine.cdromPresent()))
      .toBe(true);

    await ejectCdrom(page);
    await expect(bayCd).not.toHaveClass(/loaded/);
    await expect(label).toHaveClass(/empty/);
    await expect(bayCd.locator('[data-role="eject"]')).toBeDisabled();
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.machine.cdromPresent()))
      .toBe(false);
  });

  test("swapping discs is independent of power state -- works while the machine is off", async ({ page }) => {
    // A real CD-ROM tray opens/closes with or without power, just like the
    // floppy bay -- see app.js's pendingCdrom.
    await boot(page);

    await page.locator("#powerSwitch").click({ force: true });
    await expect(page.locator("#powerLed")).not.toHaveClass(/power-on/);

    const bayCd = bay(page, "cdrom");
    const isoPath = makeBlankIso(2048 * 16);
    await insertCdrom(page, isoPath);
    await expect(bayCd).toHaveClass(/loaded/);

    await ejectCdrom(page);
    await expect(bayCd).not.toHaveClass(/loaded/);
  });
});
