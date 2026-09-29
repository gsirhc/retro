import { test, expect } from "./fixtures";
import { boot, bootLive, waitForScreen, setPowerSwitch } from "./helpers";

// Hard disk (C: fixed drive) controls: reset/blank/download/upload operations
// take effect only on next power-on since a real WD1003 can't be swapped live.
// Buttons disabled while running, upload/reset/blank also disabled until firmware
// loads. C: state persists across reloads via IndexedDB.
test.describe("hard disk", () => {
  test("shows the factory-default label and correct button states on first load", async ({
    page,
  }) => {
    // expectScreen: null -- assert on first load, before the machine has
    // finished booting, which is what this test is actually about. Waiting
    // for the C:\> prompt would defeat it: FreeDOS genuinely writes to C:
    // while running FDAUTO.BAT, ~2s of real time BEFORE the prompt appears,
    // so by then persistHddIfDirty()'s 5s tick has correctly relabelled the
    // drive "saved state (changes from this session)". See PC486_REVIEW.md §8.
    await bootLive(page);
    await expect(page.locator("#hddStatus")).toHaveText(
      /Using: factory FreeDOS \(default\)/
    );
    await expect(page.locator("#hddDownloadBtn")).toBeEnabled();
    await expect(page.locator("#hddResetBtn")).toBeDisabled();
    await expect(page.locator("#hddBlankBtn")).toBeDisabled();
    await expect(page.locator("#hddUploadInput")).toBeDisabled();
  });

  test("Reset to factory FreeDOS and Mount blank drive are only usable while powered off, and update the status label", async ({
    livePage: page,
  }) => {
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
    livePage: page,
  }) => {
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

    // Put factory FreeDOS back so the shared livePage isn't left on a blank
    // image (status becomes "saved state" after mount, which resetLivePage
    // cannot distinguish from a normal dirty factory disk).
    await setPowerSwitch(page, false);
    await page.locator("#hddResetBtn").click();
    await setPowerSwitch(page, true);
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, {
      timeout: 15000,
    });
  });

  test("Download image is enabled even while the machine is running", async ({
    livePage: page,
  }) => {

    await expect(page.locator("#hddDownloadBtn")).toBeEnabled();

    const [download] = await Promise.all([
      page.waitForEvent("download"),
      page.locator("#hddDownloadBtn").click(),
    ]);
    expect(download.suggestedFilename()).toBe("pc486-hdd.img");
  });

  test("C: persists across a page reload via IndexedDB", async ({
    page,
  }) => {
    await boot(page);
    // Flush before power-off so the 504MB IndexedDB put isn't racing the
    // reload (persistHddIfDirty used to fire-and-forget saveHdd).
    await page.evaluate(async () => {
      await (window as any).__test.persistHdd();
      await (window as any).__test.whenHddSaved();
    });
    await setPowerSwitch(page, false);
    await page.evaluate(() => (window as any).__test.whenHddSaved());
    await page.reload();

    await page.waitForFunction(() => !!(window as any).__test?.machine, null, {
      timeout: 15000,
    });
    await expect(page.locator("#hddStatus")).toBeVisible();
    await waitForScreen(page, /C:\\>/);
  });

  // Mirrors the CD-ROM "never fetches unasked" case: once C: lives in
  // IndexedDB, a reload must not pull disks/freedos-hdd.img again. The
  // previous eager fetch started that download on every visit and only
  // skipped *awaiting* it when saved state existed.
  test("a reload with saved C: does not re-fetch the factory FreeDOS image", async ({
    page,
  }) => {
    await boot(page);
    await page.evaluate(async () => {
      await (window as any).__test.persistHdd();
      await (window as any).__test.whenHddSaved();
    });
    await expect(page.locator("#hddStatus")).toHaveText(/saved state/);
    await setPowerSwitch(page, false);
    await page.evaluate(() => (window as any).__test.whenHddSaved());

    const requests: string[] = [];
    page.on("request", (req) => requests.push(req.url()));
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, {
      timeout: 15000,
    });
    await expect(page.locator("#hddStatus")).toHaveText(/saved state/);
    expect(requests.some((u) => u.includes("freedos-hdd.img"))).toBe(false);
  });
});
