import { test, expect } from "./fixtures";
import {
  boot,
  regs,
  memAt,
  setSwitches,
  clickPaddle,
  panelStop,
  panelRun,
} from "./helpers";

const leds = (page) => page.evaluate(() => (window as any).__test.leds());
const status = (page) => leds(page).then((l) => l.status);

test.describe("front-panel lamps", () => {
  test("the address switches decode every one of the 16 bit positions", async ({ page }) => {
    await boot(page);
    await panelStop(page);
    for (let bit = 0; bit < 16; bit++) {
      const addr = 1 << bit;
      await setSwitches(page, addr);
      await clickPaddle(page, /EXAMINE/, "up");
      expect((await regs(page)).pc).toBe(addr); // switch bit N -> address bit N
      await expect.poll(async () => (await leds(page)).addr).toBe(addr); // ...and the lamp
    }
    for (const word of [0x0000, 0xffff, 0xaaaa, 0x5555, 0xcafe]) {
      await setSwitches(page, word);
      await clickPaddle(page, /EXAMINE/, "up");
      expect((await regs(page)).pc).toBe(word);
    }
  });

  test("the data lamps show the byte at the examined address", async ({ page }) => {
    await boot(page);
    await panelStop(page);

    await setSwitches(page, 0x0800);
    await clickPaddle(page, /EXAMINE/, "up");
    await expect.poll(async () => (await leds(page)).data).toBe(0); // fresh RAM reads 0

    await setSwitches(page, 0x00c3);
    await clickPaddle(page, /DEPOSIT/, "up");
    await expect.poll(async () => (await leds(page)).data).toBe(0xc3); // deposited byte on the lamps

    await setSwitches(page, 0x0801);
    await clickPaddle(page, /EXAMINE/, "up");
    await expect.poll(async () => (await leds(page)).data).toBe(0); // move on -> the next cell

    await setSwitches(page, 0x0800);
    await clickPaddle(page, /EXAMINE/, "up");
    await expect.poll(async () => (await leds(page)).data).toBe(0xc3); // ...and back
  });

  test("DEPOSIT NEXT walks a pattern into 32 bytes and the lamps track it", async ({ page }) => {
    await boot(page);
    await panelStop(page);
    await setSwitches(page, 0x0700);
    await clickPaddle(page, /EXAMINE/, "up");
    for (let i = 0; i < 32; i++) {
      await setSwitches(page, (0x40 + i) & 0xff);
      await clickPaddle(page, /DEPOSIT/, i === 0 ? "up" : "down");
    }
    for (let i = 0; i < 32; i++) expect(await memAt(page, 0x0700 + i)).toBe((0x40 + i) & 0xff);
    await expect.poll(async () => (await leds(page)).data).toBe(0x5f); // last byte on the lamps
  });

  test("WAIT is lit while the CPU is stopped and clears on RUN", async ({ page }) => {
    await boot(page);
    expect((await status(page)).WAIT).toBe(false); // running
    await panelStop(page);
    await expect.poll(async () => (await status(page)).WAIT).toBe(true);
    await panelRun(page);
    await expect.poll(async () => (await status(page)).WAIT).toBe(false);
  });

  test("MEMR / M1 are lit while running and dark once the CPU halts (+ WAIT with HLTA)", async ({
    page,
  }) => {
    await boot(page);
    expect((await status(page)).MEMR).toBe(true); // the echo ROM is fetching
    expect((await status(page)).M1).toBe(true);

    // key in a HLT at 0 and run into it
    await panelStop(page);
    await setSwitches(page, 0x0000);
    await clickPaddle(page, /EXAMINE/, "up");
    await setSwitches(page, 0x0076); // HLT
    await clickPaddle(page, /DEPOSIT/, "up");
    await setSwitches(page, 0x0000);
    await clickPaddle(page, /EXAMINE/, "up");
    await panelRun(page);

    await expect.poll(async () => (await status(page)).HLTA).toBe(true);
    const st = await status(page);
    expect(st.MEMR).toBe(false); // bus idle -- nothing fetching
    expect(st.M1).toBe(false);
    expect(st.WAIT).toBe(true); // halted counts as waiting
  });

  test("INTE follows EI / DI", async ({ page }) => {
    await boot(page);
    await panelStop(page);
    await setSwitches(page, 0x0000);
    await clickPaddle(page, /EXAMINE/, "up");
    for (const b of [0xf3, 0xfb, 0x76]) {
      // DI ; EI ; HLT
      await setSwitches(page, b);
      await clickPaddle(page, /DEPOSIT/, b === 0xf3 ? "up" : "down");
    }
    await setSwitches(page, 0x0000);
    await clickPaddle(page, /EXAMINE/, "up");

    await clickPaddle(page, /SINGLE STEP/); // DI
    await expect.poll(async () => (await status(page)).INTE).toBe(false);
    await clickPaddle(page, /SINGLE STEP/); // EI
    await expect.poll(async () => (await status(page)).INTE).toBe(true);
  });

  test("INP is lit while the paper-tape reader is feeding", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await page.evaluate(() => (window as any).__test.paperTape.setSpeed("realistic")); // a slow read to observe
    await expect(page.locator("#ptr")).toHaveClass(/loaded/);
    await page.click("#ptr .ptr-load");
    await expect.poll(async () => (await status(page)).INP, { timeout: 10_000 }).toBe(true);
    // and it goes out once the reader stops
    await page.evaluate(() => (window as any).__test.paperTape.setSpeed("max"));
    await expect.poll(async () => (await status(page)).INP, { timeout: 20_000 }).toBe(false);
  });

  test("Kill the Bit: the matching sense switch clears the moving bit, a wrong one adds one", async ({
    page,
  }) => {
    await boot(page, { params: "preset=baremetal" });
    await expect(page.locator("#ptr")).toHaveClass(/loaded/, { timeout: 10_000 });
    await page.click("#ptr .ptr-load");

    // the single bit marches across A8..A15 -- visible on every frame (the blur)
    await expect.poll(async () => (await leds(page)).addr & 0xff00, { timeout: 12_000 }).toBeGreaterThan(0);
    const seen = new Set<number>();
    for (let i = 0; i < 8; i++) {
      seen.add((await leds(page)).addr & 0xff00);
      await page.waitForTimeout(150);
    }
    expect(seen.size).toBeGreaterThan(2); // marching, not frozen

    // stop, read the bit, aim the matching sense switch under it, step it out
    // (IN 0FFh / XRA D / RRC / MOV D,A -- the XOR clears D)
    await panelStop(page);
    const d = (await regs(page)).d;
    expect(d).toBeGreaterThan(0);
    expect(d & (d - 1)).toBe(0); // exactly one bit

    await setSwitches(page, 0x0010); // EXAMINE the IN 0FFh
    await clickPaddle(page, /EXAMINE/, "up");
    await setSwitches(page, d << 8); // A8..A15 switch row = the bit's position
    for (let i = 0; i < 4; i++) await clickPaddle(page, /SINGLE STEP/);
    expect((await regs(page)).d).toBe(0); // killed

    // a switch that doesn't match XORs a bit back in
    await setSwitches(page, 0x0010);
    await clickPaddle(page, /EXAMINE/, "up");
    await setSwitches(page, 0x0100);
    for (let i = 0; i < 4; i++) await clickPaddle(page, /SINGLE STEP/);
    expect((await regs(page)).d).not.toBe(0);
  });
});
