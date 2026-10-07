import { test, expect } from "@playwright/test";

// End-to-end demo: enter the shell (SHELL_ENTRY from window.CGOAC_ENTRYPOINTS),
// type a program, LIST, ASM, RUN.

// driveFrame paces input to the ACIA baud, so type one line and let the frame
// loop drain it before the next
async function typeLine(page: import("@playwright/test").Page, text: string) {
  await page.keyboard.type(text, { delay: 100 });
  await page.keyboard.press("Enter");
  await page.waitForTimeout(300);
}

test("type, LIST, ASM, and RUN a program entirely through the terminal", async ({ page }) => {
  await page.goto("/");
  await expect(page.locator("#screen .xterm-rows")).toContainText("\\", { timeout: 45000 });
  await page.click("#screen");

  const E = await page.evaluate(() => (window as any).CGOAC_ENTRYPOINTS);

  // Enter the shell
  await typeLine(page, `${E.SHELL_ENTRY.toString(16).toUpperCase()}R`);
  await expect(page.locator("#screen .xterm-rows")).toContainText("6502 ASSEMBLY CODER", { timeout: 20000 });

  // one explicitly numbered out of order to prove the shell sorts by number
  await typeLine(page, "20 STA $50");
  await typeLine(page, "10 START: LDA #$2A");
  await typeLine(page, "30 JMP START");

  // compare positions only within LIST's reply (after the last ">LIST" echo),
  // since the earlier typed lines are echoed above it
  await typeLine(page, "LIST");
  // PRINT_ENTRY reformats into fixed columns (editor.s)
  await expect(page.locator("#screen .xterm-rows")).toContainText("10 START:  LDA #$2A", { timeout: 20000 });
  const screenText = await page.locator("#screen .xterm-rows").innerText();
  const listReply = screenText.slice(screenText.lastIndexOf(">LIST"));
  expect(listReply.indexOf("10 START")).toBeLessThan(listReply.indexOf("20         STA"));
  expect(listReply.indexOf("20         STA")).toBeLessThan(listReply.indexOf("30         JMP"));

  // ASM
  await typeLine(page, "ASM");
  await expect(page.locator("#screen .xterm-rows")).toContainText("Ok", { timeout: 20000 });

  // JMP START loops forever, which proves the object code is real 65C02
  await typeLine(page, "RUN");
  await page.waitForTimeout(500);
  const s1 = await page.evaluate(() => window.__machine.cycleCount());
  await page.waitForTimeout(500);
  const s2 = await page.evaluate(() => window.__machine.cycleCount());
  expect(s2).toBeGreaterThan(s1);
});

test("Ctrl-C breaks a genuinely hung (infinite-loop) program back to the shell", async ({ page }) => {
  await page.goto("/");
  await expect(page.locator("#screen .xterm-rows")).toContainText("\\", { timeout: 45000 });
  await page.click("#screen");

  const E = await page.evaluate(() => (window as any).CGOAC_ENTRYPOINTS);
  await typeLine(page, `${E.SHELL_ENTRY.toString(16).toUpperCase()}R`);
  await expect(page.locator("#screen .xterm-rows")).toContainText("6502 ASSEMBLY CODER", { timeout: 20000 });

  // tight infinite loop; ROM-level twin is editor_test.cpp's Ctrl-C test
  await typeLine(page, "10 START: JMP START");
  await typeLine(page, "ASM");
  await expect(page.locator("#screen .xterm-rows")).toContainText("Ok", { timeout: 20000 });

  await typeLine(page, "RUN");
  await page.waitForTimeout(500);
  const s1 = await page.evaluate(() => window.__machine.cycleCount());
  await page.waitForTimeout(500);
  const s2 = await page.evaluate(() => window.__machine.cycleCount());
  expect(s2).toBeGreaterThan(s1);   // genuinely spinning, not stalled elsewhere

  const screenBefore = await page.locator("#screen .xterm-rows").innerText();
  const promptsBefore = (screenBefore.match(/>/g) || []).length;

  // xterm.js sends ETX ($03) for Ctrl-C with no selection, which NMI_HANDLER recognizes
  await page.keyboard.press("Control+C");
  await page.waitForTimeout(1000);

  const screenAfter = await page.locator("#screen .xterm-rows").innerText();
  const promptsAfter = (screenAfter.match(/>/g) || []).length;
  expect(promptsAfter).toBeGreaterThan(promptsBefore);   // a fresh ">" landed

  // LIST still works
  await typeLine(page, "LIST");
  await expect(page.locator("#screen .xterm-rows")).toContainText("10 START: JMP START", { timeout: 20000 });
});

test("backspace erases the character, not just the cursor", async ({ page }) => {
  // checked on the raw echoed bytes (Machine.readOutput), not DOM text; ROM-level
  // twin is editor_test.cpp
  await page.goto("/");
  await expect(page.locator("#screen .xterm-rows")).toContainText("\\", { timeout: 45000 });
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
  await expect(page.locator("#screen .xterm-rows")).toContainText("6502 ASSEMBLY CODER", { timeout: 20000 });

  await page.keyboard.type("10 LDA", { delay: 100 });
  await page.keyboard.press("Backspace");   // erase the trailing 'A'
  await page.waitForTimeout(300);

  // READCHAR's BS echo, then READLINE_ECHO's space and second BS
  await expect
    .poll(() => page.evaluate(() => (window as any).__rawOut), { timeout: 20000 })
    .toContain("\x08 \x08");
});
