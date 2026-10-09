import { test, expect } from "./fixtures";
import { boot, bay, insertFloppy, ejectFloppy, typeStr, pressEnter, screenText, waitForScreen } from "./helpers";
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

  test("a drive's LED lights while its motor runs, not while it sits idle", async ({ page }) => {
    await boot(page);
    const ledA = bay(page, 0).locator('[data-role="led"]');
    // A:'s motor is still running down from the boot probe
    await expect.poll(() => page.evaluate(() => (window as any).__test.machine.floppyMotorOn(0))).toBe(false);
    await page.evaluate(() => {
      (window as any).__ledA = 0;
      const led = document.querySelector('.at-bay[data-drive="0"] [data-role="led"]')!;
      new MutationObserver(() => {
        if (led.classList.contains("on")) (window as any).__ledA++;
      }).observe(led, { attributes: true, attributeFilter: ["class"] });
    });
    await insertFloppy(page, 0, makeBlankImage(1228800));
    await page.waitForTimeout(1000);
    expect(await page.evaluate(() => (window as any).__ledA)).toBe(0);

    await typeStr(page, "DIR A:");
    await expect.poll(() => page.evaluate(() => (window as any).__ledA), { timeout: 20_000 }).toBeGreaterThan(0);
    await expect(bay(page, 1).locator('[data-role="led"]')).not.toHaveClass(/\bon\b/);
    // the BIOS turns the motor off about 2s after the last access
    await expect(ledA).not.toHaveClass(/\bon\b/, { timeout: 30_000 });
  });

  test("FORMAT B: formats a 360KB diskette end to end", async ({ page }) => {
    test.setTimeout(300_000);
    await boot(page);
    // an MS-DOS 360KB FAT12 disk with nothing on it
    const img = Buffer.alloc(368640, 0xf6);
    const boot0 = Buffer.alloc(512, 0);
    boot0.set([0xeb, 0x3c, 0x90], 0);
    boot0.write("MSDOS5.0", 3, "ascii");
    boot0.writeUInt16LE(512, 11);
    boot0[13] = 2;
    boot0.writeUInt16LE(1, 14);
    boot0[16] = 2;
    boot0.writeUInt16LE(112, 17);
    boot0.writeUInt16LE(720, 19);
    boot0[21] = 0xfd;
    boot0.writeUInt16LE(2, 22);
    boot0.writeUInt16LE(9, 24);
    boot0.writeUInt16LE(2, 26);
    boot0[510] = 0x55;
    boot0[511] = 0xaa;
    boot0.copy(img, 0);
    img.fill(0, 512, 512 * 12);
    for (const fat of [1, 3]) img.set([0xfd, 0xff, 0xff], fat * 512);
    const p = path.join(os.tmpdir(), `ibmpcat-fmt-${Date.now()}.img`);
    fs.writeFileSync(p, img);
    await insertFloppy(page, 1, p);
    // FreeDOS FORMAT 0.91v needs DOS to have read the drive once (IBM_PCAT_PARITY.md S5)
    await typeStr(page, "DIR B:");
    await waitForScreen(page, /File not found/, 60_000);
    await typeStr(page, "FORMAT B: /U /V:BLANK");
    await waitForScreen(page, /Press ENTER/i, 30_000);
    await pressEnter(page);
    await waitForScreen(page, /Format another/, 240_000);
    expect(await screenText(page)).toMatch(/368,640\s+bytes total disk space/);
    await typeStr(page, "N");
    await waitForScreen(page, /C:\\>\s*$/, 30_000);
    await typeStr(page, "DIR B:");
    await waitForScreen(page, /Volume in drive B is BLANK/, 30_000);
    const tail = await page.evaluate(() => {
      const img = (window as any).__test.machine.floppyImage(1);
      return img[img.length - 1];
    });
    expect(tail).toBe(0xf6);
  });
});
