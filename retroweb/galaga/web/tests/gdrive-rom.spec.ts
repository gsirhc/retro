import { test, expect } from "@playwright/test";

// Google Drive ROM picker. Nothing here contacts Google -- the suite
// covers the legal-disclaimer dialog and the unconfigured (no Picker API
// key / ?gdrive_unconfigured=1) path, the same rule as ibmpc-at's
// gdrive.spec.ts.

const ACK = "retroweb.romDriveLegalAck";

test.describe("Google Drive ROM load", () => {
  test("#loadRomDriveBtn is on the ROM sockets row", async ({ page }) => {
    await page.goto("/?test=1");
    await expect(page.locator("#loadRomDriveBtn")).toBeVisible();
    await expect(page.locator("#loadRomDriveBtn")).toHaveText(/Load ROM from Google Drive/i);
    // drive.file downloads 404 unless the Picker is bound to this Cloud project.
    expect(await page.evaluate(() => (window as any).RetroGdrive.APP_ID)).toBe("610038606273");
  });

  test("first click opens the legal disclaimer; Cancel does not ack", async ({ page }) => {
    await page.goto("/?test=1");
    await expect.poll(() => page.evaluate(() => !!(window as any).__test)).toBe(true);
    await page.evaluate((k) => localStorage.removeItem(k), ACK);
    await page.locator("#loadRomDriveBtn").click();
    await expect(page.locator("#romDriveLegalHint")).toBeVisible();
    await expect(page.locator("#romDriveLegalHint")).toContainText(/stays in this browser only/i);
    await page.locator("#romDriveLegalCancel").click();
    await expect(page.locator("#romDriveLegalHint")).not.toBeVisible();
    expect(await page.evaluate((k) => localStorage.getItem(k), ACK)).toBeNull();
  });

  test("OK acks once; a later click does not reopen the dialog", async ({ page }) => {
    await page.goto("/?test=1");
    await expect.poll(() => page.evaluate(() => !!(window as any).__test)).toBe(true);
    await page.evaluate((k) => localStorage.removeItem(k), ACK);
    await page.locator("#loadRomDriveBtn").click();
    await expect(page.locator("#romDriveLegalHint")).toBeVisible();
    await page.locator("#romDriveLegalOk").click();
    await expect(page.locator("#romDriveLegalHint")).not.toBeVisible();
    expect(await page.evaluate((k) => localStorage.getItem(k), ACK)).toBe("1");
    await page.locator("#loadRomDriveBtn").click();
    await expect(page.locator("#romDriveLegalHint")).not.toBeVisible();
  });

  test("unconfigured click after ack reports not set up yet", async ({ page }) => {
    await page.goto("/?test=1&gdrive_unconfigured=1");
    await expect.poll(() => page.evaluate(() => !!(window as any).__test)).toBe(true);
    await page.evaluate((k) => localStorage.setItem(k, "1"), ACK);
    await page.locator("#loadRomDriveBtn").click();
    await expect(page.locator("#romStatus")).toHaveText(/not set up yet/i);
    await expect(page.locator("#romDriveLegalHint")).not.toBeVisible();
  });

  // Stub GIS so OK's click can be shown to start requestAccessToken
  // without a real Google window. The real gsi/client script is blocked
  // so it cannot overwrite the stub.
  test("OK click starts the Google sign-in token request", async ({ page }) => {
    await page.route("https://accounts.google.com/**", (route) => route.abort());
    await page.route("https://apis.google.com/**", (route) => route.abort());
    await page.addInitScript(() => {
      (window as any).__gdriveAllowTokenInTest = true;
      (window as any).__gisPrompts = [];
      (window as any).google = {
        accounts: {
          oauth2: {
            initTokenClient() {
              return {
                callback: () => {},
                error_callback: () => {},
                requestAccessToken(opts: { prompt?: string }) {
                  (window as any).__gisPrompts.push(opts);
                },
              };
            },
          },
        },
      };
    });
    await page.goto("/?test=1");
    await expect.poll(() => page.evaluate(() => !!(window as any).__test)).toBe(true);
    await page.evaluate((k) => localStorage.removeItem(k), ACK);
    await page.locator("#loadRomDriveBtn").click();
    await expect(page.locator("#romDriveLegalHint")).toBeVisible();
    await page.locator("#romDriveLegalOk").click();
    await expect(page.locator("#romDriveLegalHint")).not.toBeVisible();
    await expect.poll(() => page.evaluate(() => (window as any).__gisPrompts.length)).toBe(1);
    expect(await page.evaluate(() => (window as any).__gisPrompts[0])).toEqual({ prompt: "" });
  });
});
