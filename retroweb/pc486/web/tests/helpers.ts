import { Page, expect } from "@playwright/test";

// Shared helpers. `window.__test = { machine, sendKey, screenEl }` (only under `?test=1`, see
// app.js) is the inspection seam. Output is a VGA canvas, so `screenText()` reads the text-mode
// screen via Machine::textScreen(), a test-only convenience in wasm_machine.cpp.

export const TEST_QS = "test=1";
export const PC486_FULL = "c4e8a1f07b2d49e6a3c58f1d6e90b274";

export function withFull(path: string): string {
  return path + (path.includes("?") ? "&" : "?") + "full=" + PC486_FULL;
}

// Navigates with `?test=1` and waits for the machine. By default also waits for the `C:\>` prompt, which is
// host-bound wall clock even under fast=1 (PC486_REVIEW.md §8.6). `expectScreen: null` skips it.
export async function boot(
  page: Page,
  opts: { params?: string; expectScreen?: RegExp | null; timeout?: number; realtime?: boolean } = {},
): Promise<void> {
  // Tests run the guest CPU faster (`fast=1`, app.js TEST_CPU_MULTIPLIER) to skip a real-speed POST + FreeDOS boot.
  // `realtime: true` opts a smoke test back into real pacing. fast=1 still means as fast as the host can
  // (PC486_REVIEW.md §8.6), so prefer bootLive / `expectScreen: null` when the prompt isn't under test.
  const speed = opts.realtime ? "" : "&fast=1";
  const qs = TEST_QS + speed + (opts.params ? `&${opts.params}` : "") + `&full=${PC486_FULL}`;
  await page.goto(`/?${qs}`);
  await page.waitForFunction(() => !!(window as any).__test?.machine, null, {
    timeout: 15_000,
  });
  if (opts.expectScreen !== null) {
    await waitForScreen(page, opts.expectScreen ?? /C:\\>/, opts.timeout ?? 90_000);
  }
}

/** Machine powered on; does not wait for FreeDOS. Prefer over `boot` for UI/API tests. */
export async function bootLive(
  page: Page,
  opts: { params?: string; timeout?: number; realtime?: boolean } = {},
): Promise<void> {
  await boot(page, { ...opts, expectScreen: null });
}

/** Restore a shared live page between tests: powered on, no media, checkboxes off, not fullscreen. */
export async function resetLivePage(page: Page): Promise<void> {
  // A prior test can leave the Esc hint dialog open or the bezel fullscreen. fsEscHint's 'close'
  // listener in shared/fullscreen.js enters fullscreen, so closing a leftover dialog leaves #escBtn visible.
  await page.evaluate(() => {
    try { localStorage.setItem("retro8080.fsEscHintSeen", "2"); } catch {}
  }).catch(() => {});
  const hintWasOpen = await page.evaluate(
    () => !!(document.getElementById("fsEscHint") as HTMLDialogElement | null)?.open,
  ).catch(() => false);
  if (hintWasOpen) {
    // close() requests fullscreen; wait for that request to settle, then exit below.
    await page.evaluate(async () => {
      const bezel = document.getElementById("bezel") as any;
      const orig = bezel.requestFullscreen;
      let req: Promise<void> | undefined;
      bezel.requestFullscreen = function (...args: unknown[]) {
        req = orig.apply(this, args);
        return req;
      };
      const hint = document.getElementById("fsEscHint") as HTMLDialogElement;
      // the close event is queued, and fullscreen.js's listener (which requests fullscreen) runs before this one
      const closed = new Promise<void>((resolve) => hint.addEventListener("close", () => resolve(), { once: true }));
      hint.close();
      await closed;
      await req?.catch(() => {});
      bezel.requestFullscreen = orig;
    });
  }
  const inFs = await page.evaluate(
    () => !!(document.fullscreenElement || (document as any).webkitFullscreenElement),
  ).catch(() => false);
  if (inFs || await page.locator("#escBtn").isVisible().catch(() => false)) {
    await page.locator("#fullscreenBtn").click();
    await expect.poll(
      () => page.evaluate(() => !document.fullscreenElement && !(document as any).webkitFullscreenElement),
      { timeout: 5_000 },
    ).toBe(true);
    await expect(page.locator("#escBtn")).toBeHidden();
  }

  const power = page.locator("#powerSwitch");
  // HDD remounts only take effect while powered off.
  const hddStatus = page.locator("#hddStatus");
  const hddText = (await hddStatus.count()) ? (await hddStatus.textContent()) || "" : "";
  const needsFactoryHdd =
    /blank drive|takes effect at next power-on/i.test(hddText);
  if (needsFactoryHdd) {
    if (await power.isChecked()) await power.click({ force: true });
    const reset = page.locator("#hddResetBtn");
    if (await reset.isEnabled()) await reset.click();
  }

  if (!(await power.isChecked())) await power.click({ force: true });
  await page.waitForFunction(() => !!(window as any).__test?.machine, null, {
    timeout: 15_000,
  });

  for (const drive of ["0", "cdrom"] as const) {
    const eject = page.locator(`.at-bay[data-drive="${drive}"] [data-role="eject"]`);
    if (await eject.isEnabled()) await eject.click();
  }
  for (const id of ["#speakerEnabled", "#mouseCaptureEnabled"]) {
    const box = page.locator(id);
    if (await box.count() && (await box.isChecked())) await box.uncheck();
  }
  // Key mappings and UI prefs persist across reloads.
  await page.evaluate(() => {
    localStorage.removeItem("retro8080.pc486.keymap");
    localStorage.removeItem("retro8080.pc486.ui");
    localStorage.removeItem("retro8080.pc486.keyboardOpen");
    const t = (window as any).__test;
    if (t?.clearKeymap) t.clearKeymap();
    t?.keyboard?.releaseAll();
  });
  // A map-mode capture left running would swallow the next test's keys.
  const mapBtn = page.locator("#keymapAddBtn");
  if ((await mapBtn.count()) && (await mapBtn.getAttribute("aria-pressed")) === "true") {
    await page.keyboard.press("Escape");
  }
  // Hide the keyboard a prior test left showing (desktop default).
  const kbToggle = page.locator("#keyboardToggle");
  if ((await kbToggle.count()) && (await kbToggle.isChecked())) {
    await kbToggle.uncheck();
    await page.evaluate(() => localStorage.removeItem("retro8080.pc486.keyboardOpen"));
  }
  // Restore Turbo if a prior test left it off.
  const turbo = page.locator("#turboBtn");
  if ((await turbo.count()) && (await turbo.getAttribute("aria-pressed")) !== "true") {
    await turbo.click();
  }

  // Theme and one-shot hints live in localStorage. Boot-notice re-arm needs a power cycle; the notice
  // test does that itself. No hint.close() here: that listener enters fullscreen.
  await page.evaluate(() => {
    localStorage.removeItem("retro8080.theme");
    localStorage.removeItem("retro8080.mode");
    localStorage.removeItem("retro8080.fsEscHintSeen");
    localStorage.removeItem("retro8080.pc486BootNoticeDismissed");
  });
  const themeBtn = page.locator("#pageThemeBtn");
  if ((await themeBtn.count()) && (await themeBtn.innerText()) !== "Windows 95") {
    await themeBtn.click();
    await page.locator('#themeDialog input[name="pageThemeFamily"][value="win"]').check();
    await page.locator('#themeDialog input[name="pageThemeMode"][value="light"]').check();
    await page.locator("#themeDialogDone").click();
  }
  await page.locator("#fullscreenBtn").focus();
}

/** Like resetLivePage, then ensure a bare `C:\>` prompt (Esc clears a typed line). */
export async function resetPromptPage(page: Page): Promise<void> {
  await resetLivePage(page);
  await focusScreen(page);
  await tap(page, "Escape");
  await waitForScreen(page, /C:\\>\s*$/, 90_000);
}

/** Current VGA text-mode screen as plain text (25 rows, "" outside text mode). */
export function screenText(page: Page): Promise<string> {
  return page.evaluate(() => (window as any).__test.machine.textScreen());
}

// Polls the text-mode screen until it matches. Generous timeout: a real-speed POST + FreeDOS boot takes tens of seconds.
export async function waitForScreen(page: Page, re: RegExp, timeout = 120_000): Promise<void> {
  await expect
    .poll(() => screenText(page), { timeout, message: `screen never matched ${re}` })
    .toMatch(re);
}

/** Real elapsed time on the page's own clock. Only for measuring wall-clock rates and real-time audio. */
export async function wallWait(page: Page, ms: number): Promise<void> {
  const target = await page.evaluate((n) => performance.now() + n, ms);
  await page.waitForFunction((t) => performance.now() >= t, target);
}

/** Let the page run n animation frames, so a negative check sees a UI that had its chance to update. */
export async function rafTicks(page: Page, n = 3): Promise<void> {
  await page.evaluate(
    (count) =>
      new Promise<void>((resolve) => {
        const step = (left: number) => (left === 0 ? resolve() : requestAnimationFrame(() => step(left - 1)));
        step(count);
      }),
    n,
  );
}

/** Wait until the guest has run `ms` of emulated time. */
export async function emuWait(page: Page, ms: number): Promise<void> {
  const target = await page.evaluate((n) => {
    const m = (window as any).__test.machine;
    return m.totalCycles() + (n * m.cpuHz()) / 1000;
  }, ms);
  await page.waitForFunction((t) => (window as any).__test.machine.totalCycles() >= t, target);
}

/** Wait until every queued scan code has been sent and the guest has read the 8042's output register. */
export async function keysDrained(page: Page): Promise<void> {
  await page.waitForFunction(() => {
    const t = (window as any).__test;
    return !t.scancodesPending && (!t.machine || (t.machine.portIn(0x64) & 1) === 0);
  });
}

/** Send one key's make then break, each byte waiting until the guest has taken it (the 8042's single-byte output register). */
export async function tap(page: Page, code: string): Promise<void> {
  await page.evaluate((c) => (window as any).__test.sendKey(c, false), code);
  await keysDrained(page);
  await page.evaluate((c) => (window as any).__test.sendKey(c, true), code);
  await keysDrained(page);
}

export async function pressEnter(page: Page): Promise<void> {
  await tap(page, "Enter");
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
// Characters that need Shift on a US layout; add more as tests need them.
const SHIFTED_CHAR_TO_KEY: Record<string, string> = {
  ":": "Semicolon",
  ">": "Period",
};

// Types a DOS command line one key at a time. Ends with Enter unless `pressEnterAfter` is false.
export async function typeStr(
  page: Page,
  str: string,
  opts: { pressEnterAfter?: boolean } = {},
): Promise<void> {
  for (const raw of str.toUpperCase()) {
    const shiftedKey = SHIFTED_CHAR_TO_KEY[raw];
    const key = shiftedKey ?? CHAR_TO_KEY[raw];
    if (!key) throw new Error(`typeStr: no SET1 key mapping for character ${JSON.stringify(raw)}`);
    if (shiftedKey) {
      await page.evaluate((c) => (window as any).__test.sendKey(c, false), "ShiftLeft");
      await keysDrained(page);
    }
    await tap(page, key);
    if (shiftedKey) {
      await page.evaluate((c) => (window as any).__test.sendKey(c, true), "ShiftLeft");
      await keysDrained(page);
    }
  }
  if (opts.pressEnterAfter !== false) await pressEnter(page);
}

/** Click the CRT so it (re)gains keyboard focus. */
export async function focusScreen(page: Page): Promise<void> {
  await page.locator("#screen").click();
}

// Flips the power rocker (no-op if already there). The real input is hidden, so the span needs `force`.
export async function setPowerSwitch(page: Page, on: boolean): Promise<void> {
  const sw = page.locator("#powerSwitch");
  if ((await sw.isChecked()) !== on) await sw.click({ force: true });
}

/** Momentary front-panel Reset: pulses CPU+chipset reset; CMOS and RAM survive (machine.h reset()). */
export async function clickReset(page: Page): Promise<void> {
  await page.locator("#resetBtn").click();
}

/** Shows the drawn keyboard under the monitor (hidden by default on desktop). */
export async function openKeyboard(page: Page): Promise<void> {
  await page.locator("#keyboardToggle").check();
}

/** A drawn key on the IBM Enhanced Keyboard under the monitor. */
export function boardKey(page: Page, code: string) {
  return page.locator(`#pcKeyboard [data-key="${code}"]`);
}

/** Ctrl+Alt+Del on the drawn keyboard: Ctrl and Alt latch, Delete sends the combo. */
export async function clickCtrlAltDel(page: Page): Promise<void> {
  await openKeyboard(page);
  await boardKey(page, "ControlLeft").click();
  await boardKey(page, "AltLeft").click();
  await boardKey(page, "Delete").click();
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

/** Mounts a mixed-mode disc via its CUE sheet and companion BIN. */
export async function insertCdromCue(page: Page, cuePath: string, binPath: string): Promise<void> {
  await bay(page, "cdrom").locator('[data-role="file"]').setInputFiles([cuePath, binPath]);
}

export async function ejectCdrom(page: Page): Promise<void> {
  await bay(page, "cdrom").locator('[data-role="eject"]').click();
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
