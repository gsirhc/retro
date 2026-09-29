import { test, expect } from "./fixtures";
import { boot, bootLive, waitForScreen, setPowerSwitch, focusScreen, typeStr } from "./helpers";

// Hard disk (C: fixed drive) controls: reset/blank/download/upload operations
// take effect only on next power-on since a real WD1003 can't be swapped live.
// Buttons disabled while running, upload/reset/blank also disabled until firmware
// loads. C: state persists across reloads via IndexedDB.

/** Force a guest write to C: and wait until the dirty bit and IndexedDB mirror catch up. */
async function dirtyAndPersistHdd(page: import("@playwright/test").Page): Promise<void> {
  // FreeDOS's own boot may not leave the image dirty (FDAUTO can be
  // read-only on a fast boot), so poke a real file write before persisting.
  await focusScreen(page);
  await typeStr(page, "ECHO P>C:\\P.TXT");
  // Do not waitForScreen(/C:\\>/) here -- that regex already matches the
  // pre-command prompt and would return before the write lands.
  await expect
    .poll(
      () =>
        page.evaluate(() => {
          const t = (window as any).__test;
          const status = document.getElementById("hddStatus")?.textContent || "";
          return t.machine.hddDirty() || /saved state/.test(status);
        }),
      { timeout: 30_000, message: "guest write never dirtied C:" },
    )
    .toBe(true);
  await page.evaluate(async () => {
    await (window as any).__test.persistHdd();
    await (window as any).__test.whenHddSaved();
  });
  await expect(page.locator("#hddStatus")).toHaveText(/saved state/);
}

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
    await dirtyAndPersistHdd(page);
    await setPowerSwitch(page, false);
    await page.evaluate(() => (window as any).__test.whenHddSaved());
    await page.reload();

    await page.waitForFunction(() => !!(window as any).__test?.machine, null, {
      timeout: 15000,
    });
    await expect(page.locator("#hddStatus")).toHaveText(/saved state/);
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
    await dirtyAndPersistHdd(page);
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
