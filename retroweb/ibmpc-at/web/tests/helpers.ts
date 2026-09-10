import { Page, expect } from "@playwright/test";

// Shared helpers for the IBM PC/AT integration suite. Everything drives the
// real page; `window.__test = { machine, sendKey, screenEl }` (present only
// under `?test=1`, see app.js) is the inspection seam. Unlike altair8800/
// assembler6502 there is no serial-terminal text buffer -- this machine's
// output is an EGA-rendered canvas -- so `screenText()` reads back the
// current text-mode screen via `Machine::textScreen()`, a test-only
// convenience added to wasm_machine.cpp for exactly this purpose (see its
// own comment there: it mirrors ega_render.cpp's RenderTextScreen character
// addressing, returns "" outside text mode, and has no real-hardware
// counterpart -- a person just looks at the CRT).

export const TEST_QS = "test=1";

/**
 * Navigate to the emulator with `?test=1` and wait for the machine to be
 * live. app.js auto-boots the instant firmware finishes fetching (no power
 * switch click needed -- see the firmware-fetch block's comment), so
 * `window.__test` appearing at all already confirms that fired; by default
 * this also waits for a fresh factory boot to reach its genuine, interactive
 * `C:\>` FreeDOS prompt (see IBM_PCAT_REVIEW.md §32's verification note).
 * Pass `expectScreen: null` to skip that wait for scenarios that
 * deliberately don't reach a normal prompt (e.g. a blank/unformatted C:).
 */
export async function boot(
  page: Page,
  opts: { params?: string; expectScreen?: RegExp | null; timeout?: number } = {},
): Promise<void> {
  const qs = opts.params ? `${TEST_QS}&${opts.params}` : TEST_QS;
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

/**
 * Poll the text-mode screen until it matches. Generous default timeout: a
 * real POST + FreeDOS boot at genuine, never-sped-up 8 MHz is tens of real
 * seconds (see CLAUDE.md's "Never speed these up"), more under contention.
 */
export async function waitForScreen(page: Page, re: RegExp, timeout = 120_000): Promise<void> {
  await expect
    .poll(() => screenText(page), { timeout, message: `screen never matched ${re}` })
    .toMatch(re);
}

/**
 * Send one physical key's full make-then-break, each byte on its own real
 * wait. The emulated 8042 (i8042.h) has exactly one single-byte output
 * register, exactly like real hardware -- a second byte written before the
 * guest's IRQ1 handler has read the first just overwrites it, silently
 * dropping it. A real keyboard can't outrun that (it clocks one bit at a
 * time over a slow serial line); a test calling sendKey() twice with no
 * real elapsed time between them can, since nothing runs the machine's
 * real-time run loop (rAF-paced) in between. This is exactly the bug behind
 * IBM_PCAT_REVIEW.md §31 (Ctrl+Alt+Del) -- mirrors app.js's own F-key
 * button handler's 50ms make/break gap, and its injectScancodeSequence()'s
 * spacing for multi-byte extended-key sequences (Print Screen, Pause,
 * arrows, etc. -- see app.js). Never send two scancode bytes back to back
 * from a test without a real wait in between.
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
 * make/break gap (see tap()'s comment -- typing several keys back to back
 * from a test hits the exact same single-byte-8042-register bug). DOS's
 * command interpreter is case-insensitive, so letters always go through
 * their bare, unshifted key -- there's no need to model CapsLock/Shift for
 * case, only for the handful of symbols (":" etc.) that need it regardless
 * of case. Ends with Enter unless `pressEnterAfter` is false.
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
 * The classic warm-boot combo via the on-page button (see app.js's
 * ctrlAltDelBtn handler and IBM_PCAT_REVIEW.md §31). Needs `force` for the
 * same reason as the power switch -- an overlaying styled element.
 */
export async function clickCtrlAltDel(page: Page): Promise<void> {
  await page.locator("#ctrlAltDelBtn").click({ force: true });
}

export function bay(page: Page, drive: 0 | 1) {
  return page.locator(`.at-bay[data-drive="${drive}"]`);
}

export async function insertFloppy(page: Page, drive: 0 | 1, filePath: string): Promise<void> {
  await bay(page, drive).locator('[data-role="file"]').setInputFiles(filePath);
}

export async function ejectFloppy(page: Page, drive: 0 | 1): Promise<void> {
  await bay(page, drive).locator('[data-role="eject"]').click();
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
