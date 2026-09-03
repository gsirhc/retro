import { test, expect } from "./fixtures";
import { boot, waitForScreen, send, setSwitches } from "./helpers";

const clearScreen = (page) => page.evaluate(() => (window as any).__test.term.clear());
const tapeStatus = (page) => page.evaluate(() => (window as any).__test.machine.tapeStatus());
const screen = (page) => page.evaluate(() => (window as any).__test.screen());

// Stock CP/M 2.2 has no console type-ahead: a command sent before the last one
// finished is lost. So type it, let the echo land, then wait for the next bare
// prompt (A> from the CCP, Ok from MBASIC) before moving on.
async function runCpm(page, cmd: string) {
  await send(page, cmd + "\r");
  await page.waitForTimeout(1200); // let the echo push past the previous prompt
  await waitForScreen(page, /(A>|Ok)[ \t]*$/m, 45_000);
}

// End-to-end "does the preset actually work" smoke tests, driven the way someone
// who knows the Altair would drive it: load the media off the real devices
// (AUTO-LOAD / PLAY / BOOT), then use the machine. The CPU still runs at 2 MHz;
// `?test=1` only maxes the paper-tape / cassette load rate.

const leds = (page) => page.evaluate(() => (window as any).__test.leds());

test.describe("preset workflows — an expert session, end to end", () => {
  // these drive real BASIC / CP/M programs at a true 2 MHz, so give them room
  // when the box is busy running other specs in parallel
  test.slow();

  test("Bare-Metal Toggle: load Kill the Bit and it plays", async ({ page }) => {
    await boot(page, { params: "preset=baremetal" });
    await expect(page.locator("#ptr")).toHaveClass(/loaded/, { timeout: 10_000 });

    // load the game tape and let it run -- the lit bit sweeps up the address
    // lamps and keeps moving
    await page.click("#ptr .ptr-load");
    await expect.poll(() => leds(page).then((l) => l.addr & 0xff00), { timeout: 12_000 }).toBeGreaterThan(0);
    const marching = new Set<number>();
    for (let i = 0; i < 8; i++) {
      marching.add((await leds(page)).addr & 0xff00);
      await page.waitForTimeout(120);
    }
    expect(marching.size).toBeGreaterThan(2); // the bit is sweeping, not frozen

    // the game reads the sense switches -- setting all of A8..A15 makes it XOR
    // extra bits in, so the single marching bit becomes a spray of them
    await setSwitches(page, 0xff00);
    await expect
      .poll(async () => {
        const hi = ((await leds(page)).addr & 0xff00) >> 8;
        return hi.toString(2).split("1").length - 1; // bits set
      }, { timeout: 5000 })
      .toBeGreaterThan(1);
    // (the winning move -- match one switch to the moving bit -- is covered
    //  deterministically in panel-lights.spec.ts)
  });

  test("Stock Launch: cold-start 4K BASIC and run a program", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await expect(page.locator("#ptr")).toHaveClass(/loaded/);

    // AUTO-LOAD keys the serial bootstrap, reads the tape, and answers
    // MEMORY SIZE? / TERMINAL WIDTH? / SIN? -- exactly the cold-start ritual
    await page.click("#ptr .ptr-load");
    await waitForScreen(page, /\bOK\b/, 90_000);

    await send(page, "10 FOR I=1 TO 4\r");
    await send(page, "20 PRINT I, I*I*I\r");
    await send(page, "30 NEXT I\r");
    await send(page, "RUN\r");
    // last line of output: I = 4, I^3 = 64
    await waitForScreen(page, /\b4\b\s+\b64\b/, 40_000);
    await waitForScreen(page, /\bOK\b/, 30_000); // and it returns to the prompt
  });

  test("Cassette Hobbyist: 8K BASIC off paper tape, then CLOAD Star Trek and RUN it", async ({
    page,
  }) => {
    await boot(page, { params: "preset=cassette" });

    // 1. bring up 8K BASIC from the paper-tape reader
    await expect(page.locator("#ptr")).toHaveClass(/loaded/);
    await page.click("#ptr .ptr-load");
    await waitForScreen(page, /\bOK\b/, 90_000);
    await send(page, "PRINT 6*7\r");
    await waitForScreen(page, /\b42\b/, 30_000); // BASIC is alive

    // 2. load Super Star Trek off the cassette: type the command, then press PLAY
    await clearScreen(page);
    await send(page, 'CLOAD "S"\r');
    await page.waitForTimeout(200);
    await page.click("#acr .acr-key.play");
    await expect
      .poll(() => tapeStatus(page).then((s) => s.pos), { timeout: 30_000 })
      .toBeGreaterThan(3000); // the tape is really reading
    await waitForScreen(page, /\bOK\b/, 90_000); // ...and BASIC comes back
    await page.click("#acr .acr-key.stop");

    // 3. play it
    await clearScreen(page);
    await send(page, "RUN\r");
    await waitForScreen(page, /STAR TREK|ENTERPRISE|KLINGON|STARDATE|COMMAND/i, 60_000);
  });

  test("CP/M Workstation: boot the floppy, check the disk, run MBASIC", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await expect(page.locator("#dcdd .dcdd-bay").first()).toHaveClass(/loaded/);

    await page.click("#dcdd .dcdd-boot");
    await waitForScreen(page, /A>/, 45_000);

    // look the disk over
    await runCpm(page, "STAT");
    expect(await screen(page)).toMatch(/R\/W|SPACE:|BYTES REMAINING/i);
    await runCpm(page, "DIR");
    expect(await screen(page)).toMatch(/\bCOM\b/); // the .COM utilities are on the disk

    // run an application off the disk and use it
    await runCpm(page, "MBASIC");
    expect(await screen(page)).toMatch(/BASIC-80/i); // the MBASIC banner
    await runCpm(page, "PRINT 355/113"); // MBASIC's Ok prompt returns
    expect(await screen(page)).toMatch(/3\.14159/);
    await runCpm(page, "SYSTEM");
    expect(await screen(page)).toMatch(/A>[ \t]*$/m); // back at the CP/M prompt
  });
});
