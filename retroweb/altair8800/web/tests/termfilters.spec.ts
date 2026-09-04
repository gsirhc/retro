import { test, expect } from "./fixtures";
import { boot } from "./helpers";

// ALTAIR_REVIEW.md §4: every profile ran through xterm.js's full VT100/xterm
// parser regardless of what the real hardware could do. These test the pure
// filter functions directly (via __test.TERM_PROFILES) rather than through
// the rendered terminal, so they're fast and don't depend on paint timing.
const bytes = (s: string) => [...s].map((c) => c.charCodeAt(0));
const str = (arr: number[]) => arr.map((b) => String.fromCharCode(b)).join("");

const runFilter = (page, key: string, input: number[]) =>
  page.evaluate(
    ({ key, input }) => {
      const p = (window as any).__test.TERM_PROFILES[key];
      const f = p.filter();
      return f(input);
    },
    { key, input },
  );

test.describe("per-profile terminal output filters", () => {
  test("ASR-33: escape sequences are swallowed, not handed to xterm", async ({ page }) => {
    await boot(page);
    // ESC [ 2 J (clear screen) then "ABC" then ESC [ 3 1 m (SGR red) then "DEF"
    const input = [...bytes("\x1b[2J"), ...bytes("ABC"), ...bytes("\x1b[31m"), ...bytes("DEF")];
    const out = await runFilter(page, "tty33", input);
    expect(str(out)).toBe("ABCDEF");
  });

  test("ASR-33: CR, LF, BS, BEL, HT survive; other C0 controls don't", async ({ page }) => {
    await boot(page);
    const input = [0x41, 0x0d, 0x0a, 0x08, 0x07, 0x09, 0x42, 0x0b, 0x0c, 0x43]; // A CR LF BS BEL HT B VT FF C
    const out = await runFilter(page, "tty33", input);
    expect(out).toEqual([0x41, 0x0d, 0x0a, 0x08, 0x07, 0x09, 0x42, 0x43]); // VT and FF dropped
  });

  test("ASR-33: an escape sequence split across two filter calls is still swallowed", async ({ page }) => {
    await boot(page);
    const f = await page.evaluateHandle(() => (window as any).__test.TERM_PROFILES.tty33.filter());
    const part1 = await page.evaluate((f) => f([0x1b, 0x5b, 0x32]), f); // ESC [ 2
    const part2 = await page.evaluate((f) => f([0x4a, 0x41, 0x42]), f); // J A B  (J ends the CSI)
    expect(part1).toEqual([]);
    expect(part2).toEqual(bytes("AB"));
  });

  test("Glass TTY uses the same dumb filter as the ASR-33", async ({ page }) => {
    await boot(page);
    const out = await runFilter(page, "glasstty", [...bytes("\x1b[H\x1b[2J"), ...bytes("hi")]);
    expect(str(out)).toBe("hi");
  });

  test("VT52: cursor moves and erase get xterm's CSI bracket added", async ({ page }) => {
    await boot(page);
    // ESC A (up), ESC K (erase to eol), ESC H (home)
    const out = await runFilter(page, "vt52", [...bytes("\x1bA"), ...bytes("\x1bK"), ...bytes("\x1bH")]);
    expect(str(out)).toBe("\x1b[A\x1b[K\x1b[H");
  });

  test("VT52: direct cursor addressing (ESC Y row col, +32 each) becomes an ANSI CUP", async ({ page }) => {
    await boot(page);
    // ESC Y <row 5 + 32> <col 10 + 32> -> row/col are 0-based on VT52, 1-based in ANSI
    const out = await runFilter(page, "vt52", [0x1b, 0x59, 0x20 + 5, 0x20 + 10]);
    expect(str(out)).toBe("\x1b[6;11H");
  });

  test("VT52: keypad-mode and identify sequences have no xterm equivalent and are dropped", async ({ page }) => {
    await boot(page);
    const out = await runFilter(page, "vt52", [...bytes("\x1b="), ...bytes("\x1b>"), ...bytes("\x1bZ"), ...bytes("ok")]);
    expect(str(out)).toBe("ok");
  });

  test("ADM-3A: bare control codes for cursor moves translate to ANSI CSI", async ({ page }) => {
    await boot(page);
    // ^K up, ^L right; ^H (BS) and ^J (LF) need no translation and pass through
    const out = await runFilter(page, "adm3a", [0x08, 0x0b, 0x0c, 0x0a]);
    expect(out).toEqual([0x08, ...bytes("\x1b[A"), ...bytes("\x1b[C"), 0x0a]);
  });

  test("ADM-3A: ^Z clears the screen and homes the cursor", async ({ page }) => {
    await boot(page);
    const out = await runFilter(page, "adm3a", [0x1a]);
    expect(str(out)).toBe("\x1b[H\x1b[2J");
  });

  test("ADM-3A: ESC = row col (+32 each) becomes an ANSI CUP, matching WordStar's driver", async ({ page }) => {
    await boot(page);
    const out = await runFilter(page, "adm3a", [0x1b, 0x3d, 0x20 + 0, 0x20 + 0]); // row 0, col 0
    expect(str(out)).toBe("\x1b[1;1H");
  });

  test("VT100 and Modern profiles have no filter -- they really are ANSI-compatible", async ({ page }) => {
    await boot(page);
    const noFilter = await page.evaluate(
      () => !(window as any).__test.TERM_PROFILES.vt100g.filter
        && !(window as any).__test.TERM_PROFILES.vt100a.filter
        && !(window as any).__test.TERM_PROFILES.modern.filter,
    );
    expect(noFilter).toBe(true);
  });

  test("the ASR-33 forces CAPS LOCK on and disables the checkbox", async ({ page }) => {
    await boot(page, { params: "term=vt100g" });
    await expect(page.locator("#caps")).toBeEnabled();
    await page.selectOption("#termProfile", "tty33");
    await expect(page.locator("#caps")).toBeChecked();
    await expect(page.locator("#caps")).toBeDisabled();
    await page.selectOption("#termProfile", "vt100g");
    await expect(page.locator("#caps")).toBeEnabled();
  });
});
