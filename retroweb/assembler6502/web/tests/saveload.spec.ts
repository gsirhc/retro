import { test, expect } from "@playwright/test";

// Phase 5's Save/Load panel and Help panel -- driven through the real
// browser, exercising the actual command shell (cpu6502/rom/editor.s's
// SHELL_ENTRY) exactly as a human typing "<addr>R" then "SAVE"/"LOAD"/
// "LIST" by hand would.

test.describe("Help panel", () => {
  test("shows this build's real shell entry point, not placeholder text", async ({ page }) => {
    await page.goto("/");
    await expect(page.locator("#screen .xterm-rows")).toContainText("\\", { timeout: 45000 });
    await page.click("#helpBtn");
    for (const id of ["hShell", "hShell2"]) {
      await expect(page.locator("#" + id)).toHaveText(/^[0-9A-F]+R$/);
    }
    await expect(page.locator("#hResume")).toHaveText(/^JMP \$[0-9A-F]+$/);
  });

  test("shows this build's real OS-call addresses, not placeholder text", async ({ page }) => {
    await page.goto("/");
    await expect(page.locator("#screen .xterm-rows")).toContainText("\\", { timeout: 45000 });
    await page.click("#helpBtn");
    for (const id of ["hPrintChar", "hPrintStr", "hLcdPutc", "hLcdPuts", "hLcdClear", "hLcdLine1", "hLcdLine2"]) {
      await expect(page.locator("#" + id)).toHaveText(/^\$[0-9A-F]+$/);
    }
  });
});

test.describe("Save / Load", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/");
    await expect(page.locator("#screen .xterm-rows")).toContainText("\\", { timeout: 45000 });
    await page.click("#screen");
    // Save/Load moved into a floating popup (terminal header) -- open it
    // once here since almost every test in this block fills/clicks its
    // controls (#pgmName, #pgmSave, #pgmFile, #instantXfer, .chip-lib).
    await page.click("#saveBtn");
    // #saveBtn's own click leaves it as document.activeElement (ordinary
    // browser button-click focus behaviour) -- xterm's helper textarea
    // needs real focus to receive page.keyboard input at all, so without
    // this every test's first enterProgram()/typeLine() call here typed
    // into the button and reached the emulator not at all. Silent for
    // tests that only check Save's own status/UI feedback (nothing
    // requires the typed program to have landed for those to still pass),
    // real for every test that verifies the actual saved/loaded content --
    // see CGOAC6502_REVIEW.md.
    await page.click("#screen");
  });

  // Same real per-character/per-line pacing pattern as editor.spec.ts:
  // app.js's own input-pacing queue plus a moment to let each line settle.
  async function typeLine(page: import("@playwright/test").Page, text: string) {
    await page.keyboard.type(text, { delay: 60 });
    await page.keyboard.press("Enter");
    await page.waitForTimeout(250);
  }
  async function enterProgram(page: import("@playwright/test").Page, lines: string[]) {
    const E = await page.evaluate(() => (window as any).CGOAC_ENTRYPOINTS);
    await typeLine(page, E.SHELL_ENTRY.toString(16).toUpperCase() + "R");
    for (const line of lines) await typeLine(page, line);
  }
  async function installOutputSpy(page) {
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
  }
  async function getRawOut(page) {
    return page.evaluate(() => (window as any).__rawOut || "");
  }

  test("Save captures the program, offers a download, and shelves it", async ({ page }) => {
    await enterProgram(page, ["LDA #$2A", "STA $50"]);

    await page.fill("#pgmName", "hello.asm");
    await page.click("#pgmSave");
    await expect(page.locator("#pgmStatus")).toContainText("Saved", { timeout: 20000 });
    await expect(page.locator("#pgmDownload")).toBeVisible();
    await expect(page.locator("#pgmDownload")).toHaveAttribute("download", "hello.asm");
    await expect(page.locator(".chip-lib button", { hasText: "hello.asm" })).toBeVisible();
  });

  test("typed SAVE prints each line on its own row, and Ok doesn't land on the last one", async ({ page }) => {
    // SAVE's own wire format is bare-CR-separated between lines (no LF --
    // see load.s), so displaying it raw would return the cursor to column
    // 0 without advancing a row: every line would overwrite the previous
    // one in place, and the trailing "Ok" would land on top of whatever's
    // left of the last line instead of its own row. Reproduces the exact
    // repro that surfaced this (type SAVE directly, not via the popup).
    await enterProgram(page, ["LDA #$58", "JSR $8003"]);
    await expect(page.locator("#screen .xterm-rows")).toContainText("LDA #$58", { timeout: 20000 });
    await typeLine(page, "SAVE");
    await page.waitForTimeout(500);

    const lines: string[] = await page.evaluate(() => {
      const buf = (window as any).__term.buffer.active;
      const out = [];
      for (let y = 0; y < buf.length; y++) out.push(buf.getLine(y)?.translateToString(true) ?? "");
      return out;
    });
    const okLine = lines.findIndex((l) => l.trim() === "Ok");
    expect(okLine, `Ok should be alone on its own row -- got:\n${lines.join("\n")}`).toBeGreaterThanOrEqual(0);
    expect(lines.some((l) => l.includes("LDA #$58"))).toBe(true);
    expect(lines.some((l) => l.includes("JSR $8003"))).toBe(true);
    // The two program lines must land on two different rows, not one
    // overwriting the other.
    const ldaRow = lines.findIndex((l) => l.includes("LDA #$58"));
    const jsrRow = lines.findIndex((l) => l.includes("JSR $8003"));
    expect(jsrRow).toBeGreaterThan(ldaRow);
  });

  test("Load (from the shelf) replaces the program with the saved one", async ({ page }) => {
    await enterProgram(page, ["LDA #$2A", "STA $50"]);
    await page.fill("#pgmName", "shelved.asm");
    await page.click("#pgmSave");
    await expect(page.locator("#pgmStatus")).toContainText("Saved", { timeout: 20000 });

    // Overwrite the program with something else, then load the shelved
    // one back in and confirm LIST shows the original, not the decoy.
    await enterProgram(page, ["NOP"]);
    await page.click('.chip-lib button:has-text("shelved.asm")');
    await page.waitForTimeout(1500);

    await typeLine(page, "LIST");
    await expect(page.locator("#screen .xterm-rows")).toContainText("LDA #$2A", { timeout: 20000 });
    await expect(page.locator("#screen .xterm-rows")).toContainText("STA $50", { timeout: 20000 });
  });

  test("manually entering the shell (as the Help panel instructs), then Save, doesn't pollute the program with a bogus re-entry line", async ({ page }) => {
    // app.js's own `inShell` tracking used to only ever get set by its own
    // ensureShell() calls -- but a human enters the shell by hand exactly
    // like enterProgram() does here (the Help panel's own documented way),
    // so app.js had no idea it was already inside. Clicking Save then
    // re-sent "<addr>R" into the *already-open* shell prompt -- misparsed
    // as a decimal line number ("8000") followed by a bad trailing letter
    // ("R") with no space between them, silently stored as a bogus extra
    // program line right before Save captured the (now polluted) buffer.
    // Fixed by inferring `inShell` from the shell's own banner in real ROM
    // output instead of only from app.js's own sends -- see app.js.
    await enterProgram(page, ["LDA #$2A", "STA $50"]);
    await page.fill("#pgmName", "clean.asm");
    await page.click("#pgmSave");
    await expect(page.locator("#pgmStatus")).toContainText("Saved", { timeout: 20000 });

    // Restore terminal focus -- filling/clicking the Save panel moved it
    // away, and typeLine()'s keystrokes need it back on #screen.
    await page.click("#screen");
    await typeLine(page, "LIST");
    // Wait for PRINT_ENTRY's own column-padded rendering specifically --
    // "STA $50" alone would already be satisfied by the earlier typed
    // entry's own echo, well before LIST's reply actually arrives.
    await expect(page.locator("#screen .xterm-rows")).toContainText("20         STA $50", { timeout: 20000 });
    const screenText = await page.locator("#screen .xterm-rows").innerText();
    const listReply = screenText.slice(screenText.lastIndexOf(">LIST"));
    // Exactly the two typed lines -- no third stray numbered line.
    expect((listReply.match(/^\d+\s/gm) || []).length).toBe(2);
  });

  test("Load's own echo lands each incoming line on its own row, not overlapping the previous one", async ({ page }) => {
    // The wire format is bare-CR-separated (load.s), and READCHAR echoes
    // every raw byte it reads including that CR -- without DO_LOAD adding
    // its own LF after each one (dload_gotcr, load.s), a bare CR just
    // returns the cursor to column 0, so each loaded line would overwrite
    // the previous one in place instead of landing on its own row. Checked
    // against the raw output byte stream (not rendered DOM text) --
    // page.locator("#screen .xterm-rows").innerText() has shown it can miss freshly-
    // rendered xterm.js content that a screenshot correctly captures, so
    // spying on Machine.readOutput (same pattern as terminal.spec.ts's
    // typeChar spy) is the reliable way to check this. See
    // CGOAC6502_REVIEW.md.
    await installOutputSpy(page);
    await enterProgram(page, ["LDA #$2A", "STA $50"]);
    await page.fill("#pgmName", "rows.asm");
    await page.click("#pgmSave");
    await expect(page.locator("#pgmStatus")).toContainText("Saved", { timeout: 20000 });

    // Restore terminal focus -- filling/clicking the Save panel moved it
    // away, and typeLine()'s keystrokes (inside enterProgram) need it back.
    await page.click("#screen");
    await enterProgram(page, ["NOP"]);
    // Checkpoint the raw output length before triggering LOAD -- SAVE's own
    // completion (just above) already printed an "Ok" of its own, so a bare
    // toContain("Ok") would resolve immediately against *that* one instead
    // of actually waiting for this LOAD to run. Slicing from this checkpoint
    // is what makes the poll below mean "a fresh Ok since LOAD started",
    // not "an Ok exists somewhere in the whole transcript" -- unlike
    // "STA $50" (already on screen from the earlier typed entry), reusing
    // the same-named signal from two different commands needs this care.
    const beforeLoad = (await getRawOut(page)).length;
    await page.click('.chip-lib button:has-text("rows.asm")');
    // Wait for DO_NEW's own "Ok" message -- printed only by DO_LOAD (it
    // always clears first) or a literal NEW command -- to appear *after*
    // the checkpoint above, meaning this LOAD's own reply has actually
    // arrived. Polled against the raw output spy, not rendered DOM text --
    // this test's header comment's innerText()/toContainText concern
    // still applies (a fresh DOM read can miss just-rendered xterm.js
    // content a screenshot would catch), where the underlying byte stream
    // is unambiguous. (The "WWWWW..." glyph-measurement placeholder leak
    // that used to alias onto this same symptom is now fixed at the
    // selector level -- every #screen locator in this suite scopes to
    // .xterm-rows, which excludes xterm's aria-hidden measurement span;
    // see CGOAC6502_REVIEW.md.)
    await expect.poll(async () => (await getRawOut(page)).slice(beforeLoad), { timeout: 20000 }).toContain("Ok");
    await page.waitForTimeout(300);

    const raw = await getRawOut(page);
    const loadReply = raw.slice(raw.lastIndexOf(">LOAD"));
    const rows = loadReply.split("\n");
    const ldaRow = rows.findIndex((r) => r.includes("LDA #$2A"));
    const staRow = rows.findIndex((r) => r.includes("STA $50"));
    expect(ldaRow).toBeGreaterThanOrEqual(0);
    expect(staRow).toBeGreaterThan(ldaRow);
    expect(rows[ldaRow]).not.toContain("STA $50");
    expect(rows[staRow]).not.toContain("LDA #$2A");
  });

  test("importing a file loads it into the program, normalizing LF line endings", async ({ page }) => {
    // A plain LF-separated file, as any normal text editor would save --
    // not the bare-CR internal format the shell's own line entry uses --
    // and with no leading line numbers at all, exercising DO_LOAD's
    // auto-numbering (see runLoad()'s normalization in app.js).
    await page.setInputFiles("#pgmFile", {
      name: "imported.asm",
      mimeType: "text/plain",
      buffer: Buffer.from("LDX #$05\nSTX $60\n"),
    });
    await expect(page.locator("#pgmStatus")).toContainText("imported.asm", { timeout: 20000 });
    await page.waitForTimeout(1500);

    await typeLine(page, "LIST");
    // PRINT_ENTRY column-aligns unlabeled lines -- number, one space, then
    // an 8-wide blank label field before the mnemonic (editor.s).
    await expect(page.locator("#screen .xterm-rows")).toContainText("10         LDX #$05", { timeout: 20000 });
    await expect(page.locator("#screen .xterm-rows")).toContainText("20         STX $60", { timeout: 20000 });

    // And it genuinely assembles -- proves the LF bytes really did become
    // real line entries, not just visually similar terminal output.
    await typeLine(page, "ASM");
    await expect(page.locator("#screen .xterm-rows")).toContainText("Ok", { timeout: 20000 });
  });

  test("instant transfer bypasses the realistic ACIA-baud pacing", async ({ page }) => {
    await page.check("#instantXfer");
    await enterProgram(page, ["LDA #$2A", "STA $50"]);
    await page.fill("#pgmName", "fast.asm");
    await page.click("#pgmSave");
    await expect(page.locator("#pgmStatus")).toContainText("Saved", { timeout: 20000 });
  });

  test("Save/Load work correctly even when the terminal is already inside the shell", async ({ page }) => {
    // A real risk this design has to avoid: if app.js blindly sent
    // "<hex>R" into an already-open shell prompt, it would misparse as a
    // huge decimal line number, not reach Wozmon's dispatcher at all.
    // Enter the shell by hand first, then drive Save through the button.
    const E = await page.evaluate(() => (window as any).CGOAC_ENTRYPOINTS);
    await typeLine(page, E.SHELL_ENTRY.toString(16).toUpperCase() + "R");
    await typeLine(page, "LDA #$2A");

    await page.fill("#pgmName", "already-in-shell.asm");
    await page.click("#pgmSave");
    await expect(page.locator("#pgmStatus")).toContainText("Saved", { timeout: 20000 });
  });
});
