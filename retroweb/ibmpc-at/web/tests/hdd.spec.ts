import { test, expect } from "./fixtures";
import { boot, waitForScreen, setPowerSwitch, typeStr } from "./helpers";
import * as fs from "fs";
import * as os from "os";
import * as path from "path";

// C: controls. Changes take effect on next power-on (a WD1003 can't be swapped live),
// and C: persists across reloads via IndexedDB.
test.describe("hard disk", () => {
  test("the activity LED lights on disk access, not at an idle prompt, and goes dark at power-off", async ({
    page,
  }) => {
    await page.addInitScript(() => {
      (window as any).__ledOn = 0;
      document.addEventListener("DOMContentLoaded", () => {
        const led = document.getElementById("hddLed")!;
        new MutationObserver(() => {
          if (led.classList.contains("on")) (window as any).__ledOn++;
        }).observe(led, { attributes: true, attributeFilter: ["class"] });
      });
    });
    const ledOn = () => page.evaluate(() => (window as any).__ledOn as number);
    const resetLed = () => page.evaluate(() => { (window as any).__ledOn = 0; });

    await boot(page);
    expect(await ledOn()).toBeGreaterThan(0);

    await page.waitForTimeout(1000);
    await resetLed();
    await page.waitForTimeout(1500);
    expect(await ledOn()).toBe(0);

    await typeStr(page, "DIR C:\\FREEDOS\\BIN");
    await expect.poll(ledOn, { timeout: 10_000 }).toBeGreaterThan(0);

    await setPowerSwitch(page, false);
    await expect(page.locator("#hddLed")).not.toHaveClass(/\bon\b/);
  });

  test("shows the factory-default label and correct button states on first load", async ({
    page,
  }) => {
    await boot(page);
    await expect(page.locator("#hddStatus")).toHaveText(
      /Using: factory FreeDOS \(default\)/
    );
    await expect(page.locator("#hddDownloadBtn")).toBeEnabled();
    await expect(page.locator("#hddResetBtn")).toBeDisabled();
    await expect(page.locator("#hddBlankBtn")).toBeDisabled();
    await expect(page.locator("#hddUploadInput")).toBeDisabled();
  });

  test("Reset to factory FreeDOS and Mount blank drive are only usable while powered off, and update the status label", async ({
    page,
  }) => {
    await boot(page);
    await setPowerSwitch(page, false);

    await expect(page.locator("#hddResetBtn")).toBeEnabled();
    await expect(page.locator("#hddBlankBtn")).toBeEnabled();

    await page.locator("#hddBlankBtn").click();
    await expect(page.locator("#hddStatus")).toHaveText(
      /blank drive, unformatted/
    );
    await expect(page.locator("#hddStatus")).toHaveText(/takes effect next power-on/);

    await page.locator("#hddResetBtn").click();
    await expect(page.locator("#hddStatus")).toHaveText(
      /factory FreeDOS \(default\)/
    );
    await expect(page.locator("#hddStatus")).toHaveText(/takes effect next power-on/);
  });

  test("a blank drive takes effect next power-on and won't boot to a normal prompt", async ({
    page,
  }) => {
    await boot(page);
    await setPowerSwitch(page, false);
    await page.locator("#hddBlankBtn").click();
    await setPowerSwitch(page, true);

    await page.waitForFunction(() => !!(window as any).__test?.machine, null, {
      timeout: 15000,
    });
    await page.waitForTimeout(3000);

    expect(
      await page.evaluate(() => (window as any).__test.machine.textScreen())
    ).not.toMatch(/C:\\>/);
  });

  test("Download image is enabled even while the machine is running", async ({
    page,
  }) => {
    await boot(page);

    await expect(page.locator("#hddDownloadBtn")).toBeEnabled();

    const [download] = await Promise.all([
      page.waitForEvent("download"),
      page.locator("#hddDownloadBtn").click(),
    ]);
    expect(download.suggestedFilename()).toBe("ibmpcat-hdd.img");
  });

  test("Upload image mounts a full-size C: at the next power-on", async ({ page }) => {
    await boot(page);
    await setPowerSwitch(page, false);
    const img = fs.readFileSync("disks/freedos-hdd.img");
    img.write("UPLOADED", img.length - 8, "ascii");
    const p = path.join(os.tmpdir(), `ibmpcat-upload-${Date.now()}.img`);
    fs.writeFileSync(p, img);

    await page.locator("#hddUploadInput").setInputFiles(p);
    await expect(page.locator("#hddStatus")).toHaveText(
      new RegExp(`uploaded image \\(${path.basename(p)}\\) -- takes effect next power-on`),
    );
    await setPowerSwitch(page, true);
    await waitForScreen(page, /C:\\>/);
    const tail = await page.evaluate(() => {
      const img = (window as any).__test.machine.hddImage();
      return String.fromCharCode(...img.slice(img.length - 8));
    });
    expect(tail).toBe("UPLOADED");
  });

  test("Upload image refuses a file that isn't the drive's exact size", async ({ page }) => {
    await boot(page);
    await setPowerSwitch(page, false);
    const p = path.join(os.tmpdir(), `ibmpcat-short-${Date.now()}.img`);
    fs.writeFileSync(p, Buffer.alloc(1024 * 1024));
    const dialog = page.waitForEvent("dialog");
    await page.locator("#hddUploadInput").setInputFiles(p);
    const d = await dialog;
    expect(d.message()).toMatch(/must be exactly \d+ bytes \(733 cyl \/ 5 head \/ 17 sec\/track\)\. Not mounted\./);
    await d.dismiss();
    await expect(page.locator("#hddStatus")).toHaveText(/factory FreeDOS \(default\)/);
  });

  test("C: persists across a page reload via IndexedDB", async ({
    page,
  }) => {
    await boot(page);
    await setPowerSwitch(page, false);
    await page.reload();

    await page.waitForFunction(() => !!(window as any).__test?.machine, null, {
      timeout: 15000,
    });
    await expect(page.locator("#hddStatus")).toBeVisible();
    await waitForScreen(page, /C:\\>/);
  });
});
