import { test, expect } from "./fixtures";
import { boot, bay, insertFloppy, ejectFloppy } from "./helpers";
import * as fs from "fs";
import * as os from "os";
import * as path from "path";

function makeBlankImage(bytes: number): string {
  const p = path.join(os.tmpdir(), `pc486-test-${Date.now()}-${Math.random().toString(36).slice(2)}.img`);
  fs.writeFileSync(p, Buffer.alloc(bytes, 0xf6));
  return p;
}

test.describe("floppy drive", () => {
  test("the bay starts empty with its real capacity in the label", async ({ page }) => {
    await boot(page);

    const bayA = bay(page, 0);
    await expect(bayA).not.toHaveClass(/loaded/);
    const labelA = bayA.locator('[data-role="label"]');
    await expect(labelA).toHaveClass(/empty/);
    await expect(labelA).toContainText(/1\.44MB/);
    await expect(bayA.locator('[data-role="eject"]')).toBeDisabled();
  });

  test("inserting a diskette loads it and enables Eject", async ({ page }) => {
    await boot(page);

    const imagePath = makeBlankImage(4096);
    await insertFloppy(page, imagePath);

    const bayA = bay(page, 0);
    await expect(bayA).toHaveClass(/loaded/);

    const labelA = bayA.locator('[data-role="label"]');
    await expect(labelA).not.toHaveClass(/empty/);
    await expect(labelA).toContainText(path.basename(imagePath));

    await expect(bayA.locator('[data-role="eject"]')).toBeEnabled();

    const isPresent = await page.evaluate(() => (window as any).__test.machine.floppyPresent());
    expect(isPresent).toBe(true);
  });

  test("ejecting an unmodified diskette just empties the bay (no download)", async ({ page }) => {
    await boot(page);

    const imagePath = makeBlankImage(4096);
    await insertFloppy(page, imagePath);

    const bayA = bay(page, 0);
    await expect(bayA).toHaveClass(/loaded/);

    await ejectFloppy(page);

    await expect(bayA).not.toHaveClass(/loaded/);
    const labelA = bayA.locator('[data-role="label"]');
    await expect(labelA).toHaveClass(/empty/);
    await expect(labelA).toContainText(/1\.44MB/);
    await expect(bayA.locator('[data-role="eject"]')).toBeDisabled();
  });

  test("floppyDirty/floppyImage reflect the embind API shape", async ({ page }) => {
    await boot(page);

    const imagePath = makeBlankImage(4096);
    await insertFloppy(page, imagePath);

    const isDirty = await page.evaluate(() => (window as any).__test.machine.floppyDirty());
    expect(isDirty).toBe(false);

    const imageBytes = await page.evaluate(() => (window as any).__test.machine.floppyImage().length);
    expect(imageBytes).toBe(4096);
  });

  test("ejecting is independent of power state -- works while the machine is off", async ({ page }) => {
    // Physical floppy slots work with or without power, just like the real
    // hardware -- see app.js's pendingFloppy.
    await boot(page);

    await page.locator("#powerSwitch").click({ force: true });
    await expect(page.locator("#powerLed")).not.toHaveClass(/power-on/);

    const imagePath = makeBlankImage(4096);
    await insertFloppy(page, imagePath);

    const bayA = bay(page, 0);
    await expect(bayA).toHaveClass(/loaded/);
    const labelA = bayA.locator('[data-role="label"]');
    await expect(labelA).not.toHaveClass(/empty/);

    await ejectFloppy(page);

    await expect(bayA).not.toHaveClass(/loaded/);
    await expect(labelA).toHaveClass(/empty/);
    await expect(bayA.locator('[data-role="eject"]')).toBeDisabled();
  });
});
