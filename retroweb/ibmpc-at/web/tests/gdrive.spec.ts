import { test, expect } from "./fixtures";
import { boot } from "./helpers";

// Google Drive sync (bring-your-own-storage for C: -- see app.js's
// "Google Drive sync" section and IBM_PCAT_REVIEW.md). Everything that
// actually talks to Google (signing in, finding/uploading/downloading the
// Drive file) needs a real, configured OAuth Client ID and a real Google
// account -- neither exists in this suite, nor should it: no test here
// contacts accounts.google.com or googleapis.com. What IS deterministic
// and worth covering without either: the button's enabled/label state
// machine, and that the placeholder Client ID this ships with (see
// GDRIVE_CLIENT_ID in app.js) fails *gracefully* with a clear status
// message rather than a cryptic Google error or a stuck "Syncing..." --
// exactly the state a fresh checkout is actually in before anyone sets up
// real Google Cloud credentials.

test.describe("Google Drive sync", () => {
  test("button starts as Connect Google Drive, enabled once firmware is ready", async ({ page }) => {
    await boot(page);
    await expect(page.locator("#gdriveSyncBtn")).toBeEnabled();
    await expect(page.locator("#gdriveSyncBtn")).toHaveText("Connect Google Drive…");
    await expect(page.locator("#gdriveStatus")).toHaveText("Not connected.");
  });

  test("clicking it with the placeholder Client ID fails gracefully, not silently or stuck", async ({ page }) => {
    await boot(page);
    await page.locator("#gdriveSyncBtn").click();
    await expect(page.locator("#gdriveStatus")).toHaveText(/not set up yet/i);
    // and the button itself must not be left disabled/stuck on "Syncing..."
    await expect(page.locator("#gdriveSyncBtn")).toBeEnabled();
    await expect(page.locator("#gdriveSyncBtn")).toHaveText("Connect Google Drive…");
  });

  test("disabled while powered off is not required -- Download-style controls work whether running or not", async ({ page }) => {
    // Unlike hddResetBtn/hddBlankBtn (which only affect the *next*
    // power-on and are disabled while running -- see hdd.spec.ts), a Drive
    // sync uploads *current* state and is equally meaningful whether the
    // machine is running or sitting powered off with a staged image, so it
    // should only ever be gated on firmware having loaded at all.
    await boot(page);
    await page.locator("#powerSwitch").click({ force: true });
    await expect(page.locator("#powerSwitch")).not.toBeChecked();
    await expect(page.locator("#gdriveSyncBtn")).toBeEnabled();
  });
});
