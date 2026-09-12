import { test, expect } from "./fixtures";
import { boot } from "./helpers";

// Google Drive sync (bring-your-own-storage for C: -- see app.js's
// "Google Drive sync" section and IBM_PCAT_REVIEW.md). Everything that
// actually talks to Google (signing in, finding/uploading/downloading the
// Drive file) needs a real, configured OAuth Client ID and a real Google
// account -- neither exists in this suite, nor should it: no test here
// contacts accounts.google.com or googleapis.com. What IS deterministic
// and worth covering without either: the button's enabled/label state
// machine, and that an unconfigured Client ID fails *gracefully* with a
// clear status message rather than a cryptic Google error or a stuck
// "Syncing...". This deployment's shipped GDRIVE_CLIENT_ID is now a real,
// working credential (see app.js), so the "unconfigured" scenarios below
// force that path with the test-only `gdrive_unconfigured=1` param instead
// of relying on the shipped constant -- that path stays real and worth
// covering because any fork/clone without its own Client ID is in exactly
// that state.

test.describe("Google Drive sync", () => {
  test("button starts as Connect Google Drive, enabled once firmware is ready", async ({ page }) => {
    await boot(page);
    await expect(page.locator("#gdriveSyncBtn")).toBeEnabled();
    await expect(page.locator("#gdriveSyncBtn")).toHaveText("Connect Google Drive…");
    await expect(page.locator("#gdriveStatus")).toHaveText("Not connected.");
  });

  test("clicking it while unconfigured fails gracefully, not silently or stuck", async ({ page }) => {
    await boot(page, { params: "gdrive_unconfigured=1" });
    await page.locator("#gdriveSyncBtn").click();
    await expect(page.locator("#gdriveStatus")).toHaveText(/not set up yet/i);
    // and the button itself must not be left disabled/stuck on "Syncing..."
    await expect(page.locator("#gdriveSyncBtn")).toBeEnabled();
    await expect(page.locator("#gdriveSyncBtn")).toHaveText("Connect Google Drive…");
    // the not-configured path returns before gdriveBusy is ever set, so
    // Cancel (only meaningful once an attempt is actually in flight) has
    // nothing to do here and must stay hidden
    await expect(page.locator("#gdriveCancelBtn")).toBeHidden();
  });

  test("Cancel button stays hidden until a sync attempt is actually in flight", async ({ page }) => {
    // Exercising the busy/Cancel-visible state itself needs a real,
    // in-flight sign-in or upload -- i.e. real Google network calls this
    // suite deliberately never makes (see the file header). What's left
    // that's deterministic: the button is hidden at rest, matching the
    // markup's own default `hidden` attribute.
    await boot(page);
    await expect(page.locator("#gdriveCancelBtn")).toBeHidden();
  });

  test("the concise setup help is shown inline (no external file reference) while unconfigured", async ({ page }) => {
    // A prior version of the "not set up" status pointed at
    // IBM_PCAT_REVIEW.md, which the user explicitly didn't want to have to
    // go open -- the actual how-to-configure-it steps now live in the page
    // itself, right below the button, and disappear once a real Client ID
    // is in place (see refreshGdriveControls() in app.js). This deployment
    // ships a real Client ID, so force the unconfigured path via the
    // test-only param to still exercise this (a fork without its own
    // credentials sees this help unconditionally).
    await boot(page, { params: "gdrive_unconfigured=1" });
    const help = page.locator("#gdriveSetupHelp");
    await expect(help).toBeVisible();
    await expect(help).not.toContainText("IBM_PCAT_REVIEW");
    await expect(help).toContainText("GDRIVE_CLIENT_ID");
  });

  test("the setup help is hidden once a real Client ID is configured", async ({ page }) => {
    await boot(page);
    await expect(page.locator("#gdriveSetupHelp")).toBeHidden();
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
