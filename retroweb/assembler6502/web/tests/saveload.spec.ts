import { test, expect } from "@playwright/test";

// Save/Load and Help panels, driven through the real shell (SHELL_ENTRY)
// the way a human typing "<addr>R" then SAVE/LOAD/LIST would.

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
    for (const id of ["hPrintChar", "hPrintStr", "hLcdPutc", "hLcdPuts", "hLcdClear", "hLcdLine1", "hLcdLine2", "hReadKey"]) {
      await expect(page.locator("#" + id)).toHaveText(/^\$[0-9A-F]+$/);
    }
  });
});

test.describe("Save / Load", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/");
    await expect(page.locator("#screen .xterm-rows")).toContainText("\\", { timeout: 45000 });
    await page.click("#screen");
    // open the Save/Load popup once; most tests use its controls
    await page.click("#saveBtn");
    // clicking #saveBtn leaves it focused, but xterm's helper textarea needs focus for page.keyboard input
    await page.click("#screen");
  });

  // same pacing as editor.spec.ts: app.js's input queue plus a moment per line
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
    // SAVE's wire format is bare-CR separated (load.s), so raw display would
    // return to column 0 and overwrite each line, with "Ok" landing on the last
    // one. Types SAVE directly, not via the popup.
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
    // the two lines must land on different rows
    const ldaRow = lines.findIndex((l) => l.includes("LDA #$58"));
    const jsrRow = lines.findIndex((l) => l.includes("JSR $8003"));
    expect(jsrRow).toBeGreaterThan(ldaRow);
  });

  test("Load (from the shelf) replaces the program with the saved one", async ({ page }) => {
    await enterProgram(page, ["LDA #$2A", "STA $50"]);
    await page.fill("#pgmName", "shelved.asm");
    await page.click("#pgmSave");
    await expect(page.locator("#pgmStatus")).toContainText("Saved", { timeout: 20000 });

    // overwrite the program, load the shelved one, confirm LIST shows the original
    await enterProgram(page, ["NOP"]);
    // Save and Load share this popup (setPgmMode); a chip click overwrites its
    // slot in Save mode, so switch to Load first
    await page.click("#loadBtn");
    await page.click('.chip-lib button:has-text("shelved.asm")');
    await page.waitForTimeout(1500);

    await typeLine(page, "LIST");
    await expect(page.locator("#screen .xterm-rows")).toContainText("LDA #$2A", { timeout: 20000 });
    await expect(page.locator("#screen .xterm-rows")).toContainText("STA $50", { timeout: 20000 });
  });

  test("Load (an Example) pokes real source straight into memory, prints LOAD/Ok, and still needs ASM before RUN", async ({ page }) => {
    // Example source is captured at build time (gen_example_bin.cpp, `make
    // examples-bin`) and poked into the source buffer (Machine.pokeRam), skipping
    // the serial LOAD path. pokeExample() fakes the LOAD/Ok echo via
    // Machine.injectOutput. The visitor still types ASM. The shell must already
    // be entered.
    await installOutputSpy(page);
    await enterProgram(page, []);
    await page.click("#loadBtn");
    await page.click('.chip-lib button:has-text("Hello, World!")');
    await expect(page.locator("#pgmStatus")).toContainText("type ASM", { timeout: 20000 });

    // the fake LOAD/Ok reached the terminal, including the trailing ">" reprompt
    await expect.poll(async () => await getRawOut(page), { timeout: 20000 }).toContain("LOAD");
    await expect.poll(async () => await getRawOut(page)).toContain("Ok");
    await expect.poll(async () => await getRawOut(page)).toMatch(/Ok\r\n>$/);

    // restore terminal focus to #screen, which typeLine() needs
    await page.click("#screen");

    // LIST proves the buffer was poked; hello.asm is under the 20-line page, so no --MORE--
    await typeLine(page, "LIST");
    await expect(page.locator("#screen .xterm-rows")).toContainText('.BYTE "HELLO, WORLD!"', { timeout: 20000 });

    // nothing is pre-assembled: ASM, then RUN produces the output
    await typeLine(page, "ASM");
    await expect(page.locator("#screen .xterm-rows")).toContainText("Ok", { timeout: 20000 });
    await typeLine(page, "RUN");
    await expect.poll(async () => await getRawOut(page), { timeout: 20000 }).toContain("HELLO, WORLD!");
  });

  test("manually entering the shell (as the Help panel instructs), then Save, doesn't pollute the program with a bogus re-entry line", async ({ page }) => {
    // app.js assumes the shell is already entered. Inferring it from the banner
    // could resend "<addr>R" into an open prompt and store a bogus line. This
    // proves the manual-entry-first flow is safe.
    await enterProgram(page, ["LDA #$2A", "STA $50"]);
    await page.fill("#pgmName", "clean.asm");
    await page.click("#pgmSave");
    await expect(page.locator("#pgmStatus")).toContainText("Saved", { timeout: 20000 });

    // restore terminal focus to #screen
    await page.click("#screen");
    await typeLine(page, "LIST");
    // wait for PRINT_ENTRY's column padding; "STA $50" is already on screen from the typed echo
    await expect(page.locator("#screen .xterm-rows")).toContainText("20         STA $50", { timeout: 20000 });
    const screenText = await page.locator("#screen .xterm-rows").innerText();
    const listReply = screenText.slice(screenText.lastIndexOf(">LIST"));
    // exactly the two typed lines
    expect((listReply.match(/^\d+\s/gm) || []).length).toBe(2);
  });

  test("Load's own echo lands each incoming line on its own row, not overlapping the previous one", async ({ page }) => {
    // SAVE output is bare-CR separated (load.s) and READCHAR echoes the CR, so
    // DO_LOAD adds an LF (dload_gotcr). Checked on the raw output stream via a
    // Machine.readOutput spy, since innerText() can miss freshly rendered xterm
    // content. See CGOAC6502_REVIEW.md.
    await installOutputSpy(page);
    await enterProgram(page, ["LDA #$2A", "STA $50"]);
    await page.fill("#pgmName", "rows.asm");
    await page.click("#pgmSave");
    await expect(page.locator("#pgmStatus")).toContainText("Saved", { timeout: 20000 });

    // restore terminal focus to #screen
    await page.click("#screen");
    await enterProgram(page, ["NOP"]);
    // checkpoint the raw output length: SAVE already printed an "Ok", so wait
    // for a fresh one after this point
    const beforeLoad = (await getRawOut(page)).length;
    // switch to Load mode, or the chip click would overwrite "rows.asm" with "NOP"
    await page.click("#loadBtn");
    await page.click('.chip-lib button:has-text("rows.asm")');
    // wait for DO_NEW's "Ok" (printed by DO_LOAD or NEW) after the checkpoint,
    // polling the raw output spy. Locators scope to .xterm-rows to exclude
    // xterm's "WWWW" measurement span (CGOAC6502_REVIEW.md).
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
    // plain LF-separated file with no line numbers, exercising DO_LOAD's
    // auto-numbering (runLoad() normalizes it). The shell must already be entered.
    await enterProgram(page, []);
    await page.setInputFiles("#pgmFile", {
      name: "imported.asm",
      mimeType: "text/plain",
      buffer: Buffer.from("LDX #$05\nSTX $60\n"),
    });
    await expect(page.locator("#pgmStatus")).toContainText("imported.asm", { timeout: 20000 });
    await page.waitForTimeout(1500);

    await typeLine(page, "LIST");
    // PRINT_ENTRY pads unlabeled lines: number, a space, an 8-wide blank label (editor.s)
    await expect(page.locator("#screen .xterm-rows")).toContainText("10         LDX #$05", { timeout: 20000 });
    await expect(page.locator("#screen .xterm-rows")).toContainText("20         STX $60", { timeout: 20000 });

    // it assembles, so the LF bytes became real line entries
    await typeLine(page, "ASM");
    await expect(page.locator("#screen .xterm-rows")).toContainText("Ok", { timeout: 20000 });
  });

  test("Save/Load work correctly even when the terminal is already inside the shell", async ({ page }) => {
    // app.js doesn't send "<hex>R" for Save/Load; the user enters the shell first
    const E = await page.evaluate(() => (window as any).CGOAC_ENTRYPOINTS);
    await typeLine(page, E.SHELL_ENTRY.toString(16).toUpperCase() + "R");
    await typeLine(page, "LDA #$2A");

    await page.fill("#pgmName", "already-in-shell.asm");
    await page.click("#pgmSave");
    await expect(page.locator("#pgmStatus")).toContainText("Saved", { timeout: 20000 });
  });
});
