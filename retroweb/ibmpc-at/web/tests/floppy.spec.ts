import { test, expect } from "./fixtures";
import { boot, bay, insertFloppy, ejectFloppy } from "./helpers";
import * as fs from "fs";
import * as os from "os";
import * as path from "path";

function makeBlankImage(bytes: number): string {
  const p = path.join(os.tmpdir(), `ibmpcat-test-${Date.now()}-${Math.random().toString(36).slice(2)}.img`);
  fs.writeFileSync(p, Buffer.alloc(bytes, 0xf6));
  return p;
}

test.describe("floppy drives", () => {
  test("both bays start empty with their real capacity in the label", async ({ page }) => {
    await boot(page);

    const bayA = bay(page, 0);
    const bayB = bay(page, 1);

    await expect(bayA).toHaveClass(/bay-525/);
    await expect(bayA.locator(".floppy-door")).toBeVisible();
    await expect(bayB).toHaveClass(/bay-525/);
    await expect(bayB.locator(".floppy-door")).toBeVisible();
    await expect(page.locator(".at-drives .bay-rail")).toHaveCount(1);

    await expect(bayA).not.toHaveClass(/loaded/);
    const labelA = bayA.locator('[data-role="label"]');
    await expect(labelA).toHaveClass(/empty/);
    await expect(labelA).toContainText(/1\.2MB/);
    await expect(bayA.locator('[data-role="eject"]')).toBeDisabled();

    await expect(bayB).not.toHaveClass(/loaded/);
    const labelB = bayB.locator('[data-role="label"]');
    await expect(labelB).toHaveClass(/empty/);
    await expect(labelB).toContainText(/360KB/);
    await expect(bayB.locator('[data-role="eject"]')).toBeDisabled();
  });

  test("inserting a diskette into drive A loads it and enables the latch eject", async ({ page }) => {
    await boot(page);

    const imagePath = makeBlankImage(4096);
    await insertFloppy(page, 0, imagePath);

    const bayA = bay(page, 0);
    await expect(bayA).toHaveClass(/loaded/);

    const labelA = bayA.locator('[data-role="label"]');
    await expect(labelA).not.toHaveClass(/empty/);
    await expect(labelA).toContainText(path.basename(imagePath));

    await expect(bayA.locator('[data-role="eject"]')).toBeEnabled();

    const isPresent = await page.evaluate(() => (window as any).__test.machine.floppyPresent(0));
    expect(isPresent).toBe(true);
  });

  test("ejecting an unmodified diskette just empties the bay (no download)", async ({ page }) => {
    await boot(page);

    const imagePath = makeBlankImage(4096);
    await insertFloppy(page, 1, imagePath);

    const bayB = bay(page, 1);
    await expect(bayB).toHaveClass(/loaded/);

    await ejectFloppy(page, 1);

    await expect(bayB).not.toHaveClass(/loaded/);
    const labelB = bayB.locator('[data-role="label"]');
    await expect(labelB).toHaveClass(/empty/);
    await expect(labelB).toContainText(/360KB/);
    await expect(bayB.locator('[data-role="eject"]')).toBeDisabled();
  });

  test("floppyDirty/floppyImage reflect the embind API shape", async ({ page }) => {
    await boot(page);

    const imagePath = makeBlankImage(4096);
    await insertFloppy(page, 0, imagePath);

    const isDirty = await page.evaluate(() => (window as any).__test.machine.floppyDirty(0));
    expect(isDirty).toBe(false);

    const imageBytes = await page.evaluate(() => (window as any).__test.machine.floppyImage(0).length);
    expect(imageBytes).toBe(4096);
  });

  test("ejecting is independent of power state -- works while the machine is off", async ({ page }) => {
    // slots work with or without power (app.js pendingFloppy)
    await boot(page);

    await page.locator("#powerSwitch").click({ force: true });
    await expect(page.locator("#powerLed")).not.toHaveClass(/power-on/);

    const imagePath = makeBlankImage(4096);
    await insertFloppy(page, 1, imagePath);

    const bayB = bay(page, 1);
    await expect(bayB).toHaveClass(/loaded/);
    const labelB = bayB.locator('[data-role="label"]');
    await expect(labelB).not.toHaveClass(/empty/);

    await ejectFloppy(page, 1);

    await expect(bayB).not.toHaveClass(/loaded/);
    await expect(labelB).toHaveClass(/empty/);
    await expect(bayB.locator('[data-role="eject"]')).toBeDisabled();
  });

  test("load overlay shows while a floppy image is being read", async ({ page }) => {
    await boot(page);
    await expect(page.locator("#loadOverlay")).not.toHaveClass(/visible/);

    // delay arrayBuffer so the overlay is observable
    await page.evaluate(() => {
      const orig = File.prototype.arrayBuffer;
      File.prototype.arrayBuffer = function () {
        return new Promise((resolve, reject) => {
          setTimeout(() => {
            orig.call(this).then(resolve, reject);
          }, 400);
        });
      };
    });

    const imagePath = makeBlankImage(4096);
    const done = insertFloppy(page, 0, imagePath);
    await expect(page.locator("#loadOverlay")).toHaveClass(/visible/);
    await expect(page.locator("#loadOverlayLabel")).toHaveText(/Loading floppy/);
    await done;
    await expect(page.locator("#loadOverlay")).not.toHaveClass(/visible/);
  });
});
