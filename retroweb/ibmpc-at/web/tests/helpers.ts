import { Page, expect } from "@playwright/test";

// Helpers for the PC/AT suite. window.__test (only under ?test=1) is the inspection seam;
// screenText() reads the EGA text screen via the test-only Machine::textScreen().

export const TEST_QS = "test=1";

/** Open ?test=1 and wait for the auto-boot; by default also waits for the C:\> prompt. expectScreen: null skips that. */
export async function boot(
  page: Page,
  opts: { params?: string; expectScreen?: RegExp | null; timeout?: number; realtime?: boolean } = {},
): Promise<void> {
  // fast=1 by default to skip the real ~45s POST + boot; realtime: true opts out (smoke.spec.ts)
  const speed = opts.realtime ? "" : "&fast=1";
  const qs = TEST_QS + speed + (opts.params ? `&${opts.params}` : "");
  await page.goto(`/?${qs}`);
  await page.waitForFunction(() => !!(window as any).__test?.machine, null, {
    timeout: 15_000,
  });
  if (opts.expectScreen !== null) {
    await waitForScreen(page, opts.expectScreen ?? /C:\\>/, opts.timeout ?? 120_000);
  }
}

/** Current EGA text-mode screen as plain text (25 rows, "" outside text mode). */
export function screenText(page: Page): Promise<string> {
  return page.evaluate(() => (window as any).__test.machine.textScreen());
}

/** Poll the text screen until it matches. Long default timeout for the real-speed boot. */
export async function waitForScreen(page: Page, re: RegExp, timeout = 120_000): Promise<void> {
  await expect
    .poll(() => screenText(page), { timeout, message: `screen never matched ${re}` })
    .toMatch(re);
}

/** One key's make then break, each byte with a real wait (the 8042 has a single output byte). */
export async function tap(page: Page, code: string, gapMs = 40): Promise<void> {
  await page.evaluate((c) => (window as any).__test.sendKey(c, false), code);
  await page.waitForTimeout(gapMs);
  await page.evaluate((c) => (window as any).__test.sendKey(c, true), code);
  await page.waitForTimeout(gapMs);
}

export async function pressEnter(page: Page, gapMs = 40): Promise<void> {
  await tap(page, "Enter", gapMs);
}

const CHAR_TO_KEY: Record<string, string> = {
  " ": "Space",
  "\\": "Backslash",
  ".": "Period",
  "-": "Minus",
  "/": "Slash",
};
for (let c = 0; c < 26; c++) {
  const letter = String.fromCharCode(65 + c);
  CHAR_TO_KEY[letter] = "Key" + letter;
}
for (let d = 0; d < 10; d++) CHAR_TO_KEY[String(d)] = "Digit" + d;
// characters that need Shift on a US layout
const SHIFTED_CHAR_TO_KEY: Record<string, string> = {
  ":": "Semicolon",
};

/** Type a DOS command one key at a time, then Enter unless pressEnterAfter is false. Letters go unshifted since DOS is case-insensitive. */
export async function typeStr(
  page: Page,
  str: string,
  opts: { gapMs?: number; pressEnterAfter?: boolean } = {},
): Promise<void> {
  const gapMs = opts.gapMs ?? 40;
  for (const raw of str.toUpperCase()) {
    const shiftedKey = SHIFTED_CHAR_TO_KEY[raw];
    const key = shiftedKey ?? CHAR_TO_KEY[raw];
    if (!key) throw new Error(`typeStr: no SET1 key mapping for character ${JSON.stringify(raw)}`);
    if (shiftedKey) {
      await page.evaluate((c) => (window as any).__test.sendKey(c, false), "ShiftLeft");
      await page.waitForTimeout(gapMs);
    }
    await tap(page, key, gapMs);
    if (shiftedKey) {
      await page.evaluate((c) => (window as any).__test.sendKey(c, true), "ShiftLeft");
      await page.waitForTimeout(gapMs);
    }
  }
  if (opts.pressEnterAfter !== false) await pressEnter(page, gapMs);
}

/** Click the CRT so it (re)gains keyboard focus. */
export async function focusScreen(page: Page): Promise<void> {
  await page.locator("#screen").click();
}

/** Flip the power rocker. Needs force: the real input is hidden behind a styled span. */
export async function setPowerSwitch(page: Page, on: boolean): Promise<void> {
  const sw = page.locator("#powerSwitch");
  if ((await sw.isChecked()) !== on) await sw.click({ force: true });
}

/** Ctrl-Alt-Del via the on-page button. Needs force like the power switch. */
export async function clickCtrlAltDel(page: Page): Promise<void> {
  await page.locator("#ctrlAltDelBtn").click({ force: true });
}

export function bay(page: Page, drive: 0 | 1) {
  return page.locator(`.at-bay[data-drive="${drive}"]`);
}

export async function insertFloppy(page: Page, drive: 0 | 1, filePath: string): Promise<void> {
  const b = bay(page, drive);
  // withLoad is async, so wait for the bay to show loaded
  await b.locator('[data-role="file"]').setInputFiles(filePath);
  await expect(b).toHaveClass(/loaded/);
  await expect(page.locator("#loadOverlay")).not.toHaveClass(/visible/);
}

export async function ejectFloppy(page: Page, drive: 0 | 1): Promise<void> {
  const b = bay(page, drive);
  await b.locator('[data-role="eject"]').click();
  await expect(b).not.toHaveClass(/loaded/);
}

/** Byte length of C:'s current image (factory, blank, or written-to). */
export function hddImageLength(page: Page): Promise<number> {
  return page.evaluate(() => (window as any).__test.machine.hddImage().length);
}

/** Wait for a real browser download and return it (eject/download flows). */
export async function expectDownload(
  page: Page,
  trigger: () => Promise<void>,
): Promise<import("@playwright/test").Download> {
  const [download] = await Promise.all([page.waitForEvent("download"), trigger()]);
  return download;
}
