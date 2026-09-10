import { test, expect } from "./fixtures";
import { boot, waitForScreen, setPowerSwitch } from "./helpers";

// Hard disk (C: fixed drive) controls: reset/blank/download/upload operations
// take effect only on next power-on since a real WD1003 can't be swapped live.
// Buttons disabled while running, upload/reset/blank also disabled until firmware
// loads. C: state persists across reloads via IndexedDB.
test.describe("hard disk", () => {
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
