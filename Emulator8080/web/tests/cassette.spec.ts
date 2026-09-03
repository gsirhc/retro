import { test, expect } from "./fixtures";
import { boot, waitForScreen, send, stubSavePicker, pickCatalogItem } from "./helpers";

const tapeStatus = (page) => page.evaluate(() => (window as any).__test.machine.tapeStatus());
const counter = (page) =>
  page.evaluate(() => document.querySelector("#acr .acr-counter")!.textContent);
const slow = (page, s = "realistic") =>
  page.evaluate((v) => (window as any).__test.cassette.setSpeed(v), s);
const clearScreen = (page) => page.evaluate(() => (window as any).__test.term.clear());

async function autoloadBasic(page) {
  await page.check("#autoload");
  await waitForScreen(page, /\bOK\b/, 40_000);
}
// send a command that echoes OK (NEW / LIST / RUN) and wait for a fresh one
async function cmd(page, line: string) {
  await clearScreen(page);
  await send(page, line + "\r");
  await waitForScreen(page, /\bOK\b/, 20_000);
}
// enter a numbered program line (BASIC prints nothing back)
async function typeLine(page, line: string) {
  await send(page, line + "\r");
  await page.waitForTimeout(150);
}

test.describe("88-ACR cassette deck", () => {
  test("the deck is shown on the Cassette Hobbyist preset with Star Trek in it", async ({ page }) => {
    await boot(page, { params: "preset=cassette" });
    await expect(page.locator("#acr")).not.toHaveClass(/empty/);
    await expect(page.locator("#acr")).toHaveClass(/loaded/);
    await expect(page.locator("#acr .acr-hint")).toContainText('CLOAD "S"');
  });

  test("transport keys are disabled until a tape is inserted", async ({ page }) => {
    await boot(page, { params: "preset=cpm" }); // no cassette preset media
    await page.evaluate(() => (window as any).__test.applyDevices(["cassette"]));
    await expect(page.locator("#acr .acr-key.play")).toBeDisabled();
    await expect(page.locator("#acr .acr-key.ff")).toBeDisabled();
    await page.evaluate(() => (window as any).__test.cassette.blank());
    await expect(page.locator("#acr .acr-key.play")).toBeEnabled();
    await expect(page.locator("#acr .acr-key.ff")).toBeEnabled();
  });

  test("Insert Blank Tape mounts an empty tape", async ({ page }) => {
    await boot(page, { params: "preset=cassette" });
    await page.click("#acr .acr-window");
    await expect(page.locator("#tapeDialog")).toBeVisible();
    await page.click("#tapeBlank");
    await expect(page.locator("#acr .acr-shell-label")).toHaveAttribute("data-label", /BLANK/);
    expect((await tapeStatus(page)).len).toBe(0);
  });

  test("the deck hint tracks the inserted tape's CLOAD name", async ({ page }) => {
    await boot(page, { params: "preset=cassette" });
    await expect(page.locator("#acr .acr-hint")).toContainText('CLOAD "S"');
    await page.evaluate(() => (window as any).__test.cassette.blank());
    await expect(page.locator("#acr .acr-hint")).toContainText('CLOAD "X"');
  });

  test('CLOAD "S" + PLAY streams Super Star Trek off the tape', async ({ page }) => {
    await boot(page, { params: "preset=cassette" });
    await autoloadBasic(page);
    await send(page, 'CLOAD "S"\r');
    await page.waitForTimeout(200);
    await page.click("#acr .acr-key.play");
    // the tape reads through and BASIC returns to OK
    await expect.poll(() => tapeStatus(page).then((s) => s.pos), { timeout: 30_000 }).toBeGreaterThan(1000);
    await waitForScreen(page, /\bOK\b/, 30_000);
    await send(page, "RUN\r");
    await waitForScreen(page, /STAR TREK|ENTERPRISE|KLINGON/i, 30_000);
  });

  test('CLOAD "S" at a throttled speed (25x) loads the whole 17 KB program', async ({ page }) => {
    test.slow(); // a deliberately real byte-rate read -- runs at 2 MHz, not Max
    await boot(page, { params: "preset=cassette" });
    await autoloadBasic(page);
    await slow(page, "x25"); // a real byte-rate, not the ?test Max
    await clearScreen(page);
    await send(page, 'CLOAD "S"\r');
    await page.waitForTimeout(300);
    await page.click("#acr .acr-key.play");
    // the head advances at ~750 B/s and BASIC actually keeps up -- no hang
    await expect.poll(() => tapeStatus(page).then((s) => s.pos), { timeout: 15_000 }).toBeGreaterThan(3000);
    await waitForScreen(page, /\bOK\b/, 60_000); // CLOAD returned -- it did not hang
    await page.click("#acr .acr-key.stop");
    // the real Super Star Trek listing came off the tape (list just its header)
    await clearScreen(page);
    await send(page, "LIST 90\r");
    await waitForScreen(page, /LEEDOM|MODIFICATIONS/i, 20_000);
  });

  test("PLAY just plays — the tape rolls even with no CLOAD pending", async ({ page }) => {
    await boot(page, { params: "preset=cassette" });
    await autoloadBasic(page);
    await slow(page); // realistic, so the transport is time-paced (not instant)
    expect(await counter(page)).toBe("000");

    await page.click("#acr .acr-key.play");
    await expect(page.locator("#acr .acr-key.play")).toHaveClass(/down/);
    // nobody is reading the board, but the head still advances
    await expect.poll(() => tapeStatus(page).then((s) => s.pos), { timeout: 8000 }).toBeGreaterThan(20);
    expect(await counter(page)).not.toBe("000");
    // and nothing was loaded into BASIC
    expect(await page.evaluate(() => (window as any).__test.screen())).not.toMatch(/SYNTAX ERROR/i);
  });

  test("recording appends — a second CSAVE does not wipe the first program", async ({ page }) => {
    await stubSavePicker(page);
    await boot(page, { params: "preset=cassette" });
    await autoloadBasic(page);
    await page.evaluate(() => (window as any).__test.cassette.eject(true));
    await page.evaluate(() => (window as any).__test.cassette.blank());

    await cmd(page, "NEW");
    await typeLine(page, '10 PRINT "AAA"');
    await page.click("#acr .acr-key.rec");
    await send(page, 'CSAVE "A"\r');
    await expect.poll(() => tapeStatus(page).then((s) => s.len)).toBeGreaterThan(0);
    await page.waitForTimeout(400);
    await page.click("#acr .acr-key.stop");
    const afterA = (await tapeStatus(page)).len;

    await cmd(page, "NEW");
    await typeLine(page, '20 PRINT "BBB"');
    await page.click("#acr .acr-key.rec");
    await send(page, 'CSAVE "B"\r');
    await page.waitForTimeout(600);
    await page.click("#acr .acr-key.stop");
    const afterB = (await tapeStatus(page)).len;
    expect(afterB).toBeGreaterThan(afterA); // B was appended, A is still there

    // read both back: REW, CLOAD "A" (from the top), then CLOAD "B" (head now past A)
    await page.click("#acr .acr-key.rew");
    await cmd(page, "NEW");
    await send(page, 'CLOAD "A"\r');
    await page.waitForTimeout(150);
    await page.click("#acr .acr-key.play");
    await page.waitForTimeout(1500);
    await page.click("#acr .acr-key.stop");
    await cmd(page, "LIST");
    expect(await page.evaluate(() => (window as any).__test.screen())).toMatch(/10 PRINT "AAA"/);

    await cmd(page, "NEW");
    await send(page, 'CLOAD "B"\r');
    await page.waitForTimeout(150);
    await page.click("#acr .acr-key.play");
    await page.waitForTimeout(1500);
    await page.click("#acr .acr-key.stop");
    await cmd(page, "LIST");
    expect(await page.evaluate(() => (window as any).__test.screen())).toMatch(/20 PRINT "BBB"/);
  });

  test("recording needs REC pressed; then SAVE .CAS downloads the tape", async ({ page }) => {
    await stubSavePicker(page);
    await boot(page, { params: "preset=cassette" });
    await autoloadBasic(page);
    await cmd(page, "NEW");
    await typeLine(page, '10 PRINT "TEST"');

    // blank tape, then try to save WITHOUT pressing REC
    await page.evaluate(() => (window as any).__test.cassette.eject(true));
    await page.evaluate(() => (window as any).__test.cassette.blank());
    await send(page, 'CSAVE "T"\r');
    await page.waitForTimeout(1200);
    expect((await tapeStatus(page)).len).toBe(0); // nothing recorded -- REC wasn't down
    await expect(page.locator("#acr .acr-save")).toBeHidden();

    // now press REC and save again
    await page.click("#acr .acr-key.rec");
    await send(page, 'CSAVE "T"\r');
    await expect(page.locator("#acr .acr-save")).toBeVisible({ timeout: 15_000 });
    await expect.poll(() => tapeStatus(page).then((s) => s.len)).toBeGreaterThan(0);

    await page.click("#acr .acr-save");
    await expect.poll(() => page.evaluate(() => (window as any).__savedBytes)).not.toBeNull();
    const saved = await page.evaluate(() => (window as any).__savedBytes);
    const onTape = await page.evaluate(() => Array.from((window as any).__test.machine.tapeImage()));
    expect(saved).toEqual(onTape);
  });

  test("EJECT clears the deck", async ({ page }) => {
    await boot(page, { params: "preset=cassette" });
    await page.evaluate(() => (window as any).__test.cassette.eject(true));
    await expect(page.locator("#acr")).not.toHaveClass(/loaded/);
    await expect(page.locator("#acr .acr-key.play")).toBeDisabled();
  });

  test("inserting a tape from the catalog dialog", async ({ page }) => {
    await boot(page, { params: "preset=cassette" });
    await page.evaluate(() => (window as any).__test.cassette.eject(true));
    await page.click("#acr .acr-window");
    await expect(page.locator("#tapeDialog")).toBeVisible();
    await pickCatalogItem(page, "#tapeList", /Star Trek/i);
    await page.click("#tapeInsert");
    await expect(page.locator("#tapeDialog")).toBeHidden();
    await expect(page.locator("#acr")).toHaveClass(/loaded/);
    await expect(page.locator("#acr .acr-hint")).toContainText('CLOAD "S"');
  });

  test("F.FWD winds the tape forward, paced by TAPE SPEED", async ({ page }) => {
    await boot(page, { params: "preset=cassette" });
    await slow(page); // realistic -- a timed wind, not an instant seek
    await page.click("#acr .acr-key.ff");
    await expect(page.locator("#acr .acr-key.ff")).toHaveClass(/down/);
    await expect(page.locator("#acr")).toHaveClass(/winding/);
    await expect.poll(() => tapeStatus(page).then((s) => s.pos), { timeout: 6000 }).toBeGreaterThan(200);
    const mid = (await tapeStatus(page)).pos;
    await page.click("#acr .acr-key.stop");
    await expect(page.locator("#acr .acr-key.ff")).not.toHaveClass(/down/);
    expect(mid).toBeLessThan((await tapeStatus(page)).cap); // didn't reach the end yet
  });

  test("the transport auto-stops and pops the key up at the tape ends", async ({ page }) => {
    await boot(page, { params: "preset=cassette" });
    await slow(page, "x5"); // fast enough to wind a whole tape within the test
    await page.evaluate(() => (window as any).__test.cassette.blank());

    // FF runs into the physical end, then releases
    await page.click("#acr .acr-key.ff");
    await expect(page.locator("#acr .acr-key.ff")).not.toHaveClass(/down/, { timeout: 20_000 });
    const st = await tapeStatus(page);
    expect(st.pos).toBe(st.cap);
    await expect(page.locator("#acr")).not.toHaveClass(/winding/);

    // REW runs back to the start, then releases
    await page.click("#acr .acr-key.rew");
    await expect(page.locator("#acr .acr-key.rew")).not.toHaveClass(/down/, { timeout: 20_000 });
    expect((await tapeStatus(page)).pos).toBe(0);
    await expect(page.locator("#acr")).not.toHaveClass(/rewinding/);
  });

  test("REW to the start, and STOP releases the keys", async ({ page }) => {
    await boot(page, { params: "preset=cassette" }); // ?test => Max: REW is instant
    await page.click("#acr .acr-key.play");
    await expect(page.locator("#acr .acr-key.play")).toHaveClass(/down/);
    await page.click("#acr .acr-key.stop");
    await expect(page.locator("#acr .acr-key.play")).not.toHaveClass(/down/);
    await page.click("#acr .acr-key.rew");
    expect((await tapeStatus(page)).pos).toBe(0);
  });

  test("the counter rolls over past 999 and clicking it zeros it", async ({ page }) => {
    await boot(page, { params: "preset=cassette" });
    await slow(page, "x5");
    await page.evaluate(() => (window as any).__test.cassette.blank());

    // wind forward far enough that the counter (pos/16) passes 999
    await page.click("#acr .acr-key.ff");
    await expect
      .poll(() => tapeStatus(page).then((s) => s.pos), { timeout: 20_000 })
      .toBeGreaterThan(16 * 1000);
    // counter is a 3-digit value that wrapped -- it is not pinned at 999
    await expect.poll(() => counter(page)).not.toBe("999");
    await page.click("#acr .acr-key.stop");

    // clicking the counter zeros it wherever the head sits
    await page.click("#acr .acr-counter");
    expect(await counter(page)).toBe("000");
  });

  test("the deck TAPE SPEED selector offers Realistic / 5x / 25x / 50x (no Max)", async ({ page }) => {
    await boot(page, { params: "preset=cassette" });
    await expect(page.locator("#acr .dev-speedlabel")).toHaveText("TAPE SPEED");
    const opts = await page.$$eval("#acr .acr-speed option", (os) => os.map((o) => o.value));
    expect(opts).toEqual(["realistic", "x5", "x25", "x50"]);

    await page.selectOption("#acr .acr-speed", "x50");
    expect(await page.evaluate(() => (window as any).__test.cassette.speed)).toBe("x50");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.tapespeed"))).toBe("x50");
    await page.selectOption("#acr .acr-speed", "realistic");
    expect(await page.evaluate(() => (window as any).__test.cassette.speed)).toBe("realistic");
  });

  test("a stale 'max' in localStorage falls back to Realistic", async ({ page }) => {
    await page.goto("/");
    await page.evaluate(() => localStorage.setItem("retro8080.tapespeed", "max"));
    await page.goto("/?preset=cassette");
    await expect(page.locator("#acr .acr-speed")).toHaveValue("realistic");
  });
});
