import { test, expect } from "@playwright/test";

// The end-to-end demo scenario, driven through the real browser terminal:
// enter the command shell (SHELL_ENTRY, a real built address -- read live
// from window.CGOAC_ENTRYPOINTS rather than hardcoded, so this can't go
// stale the way a hand-copied hex literal would), type a program, LIST,
// ASM, RUN.

// app.js's driveFrame paces input to the ACIA's live baud by default (see
// its own header comment) -- typing one line, then giving the frame loop a
// clear moment to fully drain it before the next line starts, keeps this
// test about the ROM's shell logic rather than that separate timing.
async function typeLine(page: import("@playwright/test").Page, text: string) {
  await page.keyboard.type(text, { delay: 100 });
  await page.keyboard.press("Enter");
  await page.waitForTimeout(300);
}

test("type, LIST, ASM, and RUN a program entirely through the terminal", async ({ page }) => {
  await page.goto("/");
  await expect(page.locator("#screen")).toContainText("\\", { timeout: 25000 });
  await page.click("#screen");

  const E = await page.evaluate(() => (window as any).CGOAC_ENTRYPOINTS);

  // Enter the shell
  await typeLine(page, `${E.SHELL_ENTRY.toString(16).toUpperCase()}R`);
  await expect(page.locator("#screen")).toContainText("6502 ASSEMBLY CODER", { timeout: 5000 });

  // Type a small program -- one explicitly numbered (out of typing order,
  // to prove the shell sorts by number, not entry order), the rest
  // auto-numbered.
  await typeLine(page, "20 STA $50");
  await typeLine(page, "10 START: LDA #$2A");
  await typeLine(page, "30 JMP START");

  // LIST -- sorted by line number, not typed order. Compare positions only
  // within LIST's own reply (after the last ">LIST" echo), not the whole
  // screen buffer -- the earlier typed-out-of-order lines ("20 STA $50"
  // before "10 START...") are themselves still echoed higher up on
  // screen and would otherwise throw off a raw indexOf comparison.
  await typeLine(page, "LIST");
  // PRINT_ENTRY reformats into fixed columns (editor.s) -- "START:" plus
  // its guaranteed separator and padding, see CGOAC6502_REVIEW.md.
  await expect(page.locator("#screen")).toContainText("10 START:  LDA #$2A", { timeout: 5000 });
  const screenText = await page.locator("#screen").innerText();
  const listReply = screenText.slice(screenText.lastIndexOf(">LIST"));
  expect(listReply.indexOf("10 START")).toBeLessThan(listReply.indexOf("20         STA"));
  expect(listReply.indexOf("20         STA")).toBeLessThan(listReply.indexOf("30         JMP"));

  // ASM
  await typeLine(page, "ASM");
  await expect(page.locator("#screen")).toContainText("Ok", { timeout: 5000 });

  // RUN -- JMP START loops forever, which is fine here: this just proves
  // the assembled object code is real, executable 65C02.
  await typeLine(page, "RUN");
  await page.waitForTimeout(500);
  const s1 = await page.evaluate(() => window.__machine.cycleCount());
  await page.waitForTimeout(500);
  const s2 = await page.evaluate(() => window.__machine.cycleCount());
  expect(s2).toBeGreaterThan(s1);
});

test("Ctrl-C breaks a genuinely hung (infinite-loop) program back to the shell", async ({ page }) => {
  await page.goto("/");
  await expect(page.locator("#screen")).toContainText("\\", { timeout: 25000 });
  await page.click("#screen");

  const E = await page.evaluate(() => (window as any).CGOAC_ENTRYPOINTS);
  await typeLine(page, `${E.SHELL_ENTRY.toString(16).toUpperCase()}R`);
  await expect(page.locator("#screen")).toContainText("6502 ASSEMBLY CODER", { timeout: 5000 });

  // A tight, genuinely infinite loop -- see bios.s's NMI_HANDLER and
  // editor_test.cpp's own Ctrl-C coverage for the ROM-level proof; this is
  // the same scenario driven through the real browser terminal instead.
  await typeLine(page, "10 START: JMP START");
  await typeLine(page, "ASM");
  await expect(page.locator("#screen")).toContainText("Ok", { timeout: 5000 });

  await typeLine(page, "RUN");
  await page.waitForTimeout(500);
  const s1 = await page.evaluate(() => window.__machine.cycleCount());
  await page.waitForTimeout(500);
  const s2 = await page.evaluate(() => window.__machine.cycleCount());
  expect(s2).toBeGreaterThan(s1);   // genuinely spinning, not stalled elsewhere

  const screenBefore = await page.locator("#screen").innerText();
  const promptsBefore = (screenBefore.match(/>/g) || []).length;

  // xterm.js's default keybinding sends the real ASCII ETX ($03) byte for
  // Ctrl-C (no text selected) -- same wire byte NMI_HANDLER recognizes.
  await page.keyboard.press("Control+C");
  await page.waitForTimeout(1000);

  const screenAfter = await page.locator("#screen").innerText();
  const promptsAfter = (screenAfter.match(/>/g) || []).length;
  expect(promptsAfter).toBeGreaterThan(promptsBefore);   // a fresh ">" landed

  // Shell is fully usable afterward -- LIST still works.
  await typeLine(page, "LIST");
  await expect(page.locator("#screen")).toContainText("10 START: JMP START", { timeout: 5000 });
});

test("backspace erases the character, not just the cursor", async ({ page }) => {
  // Checked on the raw echoed byte stream (Machine.readOutput), not
  // rendered DOM text -- this is a terminal-control-sequence claim (which
  // bytes went out for one backspace keystroke), not a content claim, and
  // editor_test.cpp's GoogleTest coverage already proves the same thing
  // at the ROM level. This is the same scenario driven through the real
  // browser terminal and input-pacing pipeline instead.
  await page.goto("/");
  await expect(page.locator("#screen")).toContainText("\\", { timeout: 25000 });
  await page.click("#screen");

  await page.evaluate(() => {
    (window as any).__rawOut = "";
    const m = (window as any).__machine;
    const orig = m.readOutput.bind(m);
    m.readOutput = () => {
      const r = orig();
      for (const b of r) (window as any).__rawOut += String.fromCharCode(b);
      return r;
    };
  });

  const E = await page.evaluate(() => (window as any).CGOAC_ENTRYPOINTS);
  await typeLine(page, `${E.SHELL_ENTRY.toString(16).toUpperCase()}R`);
  await expect(page.locator("#screen")).toContainText("6502 ASSEMBLY CODER", { timeout: 5000 });

  await page.keyboard.type("10 LDA", { delay: 100 });
  await page.keyboard.press("Backspace");   // erase the trailing 'A'
  await page.waitForTimeout(300);

  // READCHAR's own bare BS echo, then READLINE_ECHO's added erase: a
  // space (overwrites the 'A'), then a second BS (backs over the space
  // too).
  await expect
    .poll(() => page.evaluate(() => (window as any).__rawOut), { timeout: 5000 })
    .toContain("\x08 \x08");
});
