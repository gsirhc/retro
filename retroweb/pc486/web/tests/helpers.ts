import { Page, expect } from "@playwright/test";

// Shared helpers for the 486DX2-66 Gaming PC integration suite. Everything
// drives the real page; `window.__test = { machine, sendKey, screenEl }`
// (present only under `?test=1`, see app.js) is the inspection seam. Like
// ibmpc-at, this machine's output is a VGA-rendered canvas with no serial-
// terminal text buffer -- `screenText()` reads back the current text-mode
// screen via `Machine::textScreen()`, a test-only convenience in
// wasm_machine.cpp with no real-hardware counterpart.

export const TEST_QS = "test=1";

/**
 * Navigate to the emulator with `?test=1` and wait for the machine to be
 * live. app.js auto-boots the instant firmware finishes fetching (no power
 * switch click needed), so `window.__test` appearing at all already
 * confirms that fired; by default this also waits for a fresh factory boot
 * to reach its genuine, interactive `C:\>` FreeDOS prompt. Pass
 * `expectScreen: null` to skip that wait for scenarios that deliberately
 * don't reach a normal prompt (e.g. a blank/unformatted C:).
 */
export async function boot(
  page: Page,
  opts: { params?: string; expectScreen?: RegExp | null; timeout?: number; realtime?: boolean } = {},
): Promise<void> {
  // Every test in this suite runs the guest CPU sped up by default
  // (`fast=1` -- see app.js's TEST_CPU_MULTIPLIER) so it doesn't pay a real
  // POST + FreeDOS boot at genuine 66 MHz on every test. Pass
  // `realtime: true` to opt a specific (smoke) test back into genuine
  // real-speed pacing -- see tests/smoke.spec.ts and CLAUDE.md "Current
  // sanctioned overrides".
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

/** Current VGA text-mode screen as plain text (25 rows, "" outside text mode). */
export function screenText(page: Page): Promise<string> {
  return page.evaluate(() => (window as any).__test.machine.textScreen());
}

/**
 * Poll the text-mode screen until it matches. Generous default timeout: a
 * real POST + FreeDOS boot at genuine, never-sped-up 66 MHz is tens of real
 * seconds (see CLAUDE.md's "Never speed these up"), more under contention.
 */
export async function waitForScreen(page: Page, re: RegExp, timeout = 120_000): Promise<void> {
  await expect
    .poll(() => screenText(page), { timeout, message: `screen never matched ${re}` })
    .toMatch(re);
}

/**
 * Send one physical key's full make-then-break, each byte on its own real
 * wait. See ibmpc-at/web/tests/helpers.ts's identical function for the full
 * rationale (the emulated 8042's single-byte output register) -- unchanged
 * on this machine, same keyboard controller convention.
 */
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
// Characters that need Shift held on a real US keyboard layout -- add here
// as tests need more of them, rather than guessing a full US layout table.
const SHIFTED_CHAR_TO_KEY: Record<string, string> = {
  ":": "Semicolon",
};

/**
 * Type a DOS command line one real key at a time, each with its own real
 * make/break gap (see tap()'s comment). Ends with Enter unless
 * `pressEnterAfter` is false.
 */
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

/**
 * Flip the power rocker to the given state (no-op if already there). The
 * real `<input>` is shrunk to 1x1px and hidden -- the visible switch is a
 * styled sibling `<span>` (see index.html's .at-switch-group CSS) -- which
 * Playwright's actionability check treats as unclickable without `force`.
 */
export async function setPowerSwitch(page: Page, on: boolean): Promise<void> {
  const sw = page.locator("#powerSwitch");
  if ((await sw.isChecked()) !== on) await sw.click({ force: true });
}

/**
 * The momentary front-panel Reset button -- unlike ibmpc-at's genuine
 * 5170 (no front-panel reset), this machine's whole premise is a clone-era
 * tower case with a real one. Pulses CPU+chipset reset; CMOS and RAM both
 * survive (see machine.h's reset() comment).
 */
export async function clickReset(page: Page): Promise<void> {
  await page.locator("#resetBtn").click();
}

/**
 * The classic warm-boot combo via the on-page button (see app.js's
 * ctrlAltDelBtn handler).
 */
export async function clickCtrlAltDel(page: Page): Promise<void> {
  await page.locator("#ctrlAltDelBtn").click({ force: true });
}

export function bay(page: Page, drive: 0 | "cdrom") {
  return page.locator(`.at-bay[data-drive="${drive}"]`);
}

export async function insertFloppy(page: Page, filePath: string): Promise<void> {
  await bay(page, 0).locator('[data-role="file"]').setInputFiles(filePath);
}

export async function ejectFloppy(page: Page): Promise<void> {
  await bay(page, 0).locator('[data-role="eject"]').click();
}

export async function insertCdrom(page: Page, filePath: string): Promise<void> {
  await bay(page, "cdrom").locator('[data-role="file"]').setInputFiles(filePath);
}

export async function ejectCdrom(page: Page): Promise<void> {
  await bay(page, "cdrom").locator('[data-role="eject"]').click();
}

/** Fetches and mounts the shipped FreeDOS install/live CD on demand -- see
 * app.js's "Load FreeDOS CD..." button. Not fetched at page load. */
export async function loadFreedosCdrom(page: Page): Promise<void> {
  await bay(page, "cdrom").locator('[data-role="load-freedos-cd"]').click();
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
