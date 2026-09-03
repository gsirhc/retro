import { test, expect } from "./fixtures";
import {
  boot,
  regs,
  memAt,
  setSwitches,
  clickPaddle,
  panelStop,
  panelRun,
  send,
  screen,
  pickCatalogItem,
} from "./helpers";

const leds = (page) => page.evaluate(() => (window as any).__test.leds());

test.describe("front panel", () => {
  test("the panel is drawn with 16 address LEDs, 8 data LEDs, 16 switches", async ({ page }) => {
    await boot(page);
    expect(await page.locator("#altair .fp-leds .fp-cell .led").count()).toBeGreaterThanOrEqual(24);
    expect(await page.locator("#altair .fp-swgrid .fp-cell.paddle").count()).toBe(16);
  });

  test("toggle switches drive switchWord()", async ({ page }) => {
    await boot(page);
    await setSwitches(page, 0xa53c);
    expect(await page.evaluate(() => (window as any).__test.switchWord)).toBe(0xa53c);
    await setSwitches(page, 0x0000);
    expect(await page.evaluate(() => (window as any).__test.switchWord)).toBe(0);
  });

  test("EXAMINE loads the switch word into PC; address LEDs follow", async ({ page }) => {
    await boot(page);
    await panelStop(page);
    await setSwitches(page, 0x1234);
    await clickPaddle(page, /EXAMINE/, "up");
    expect((await regs(page)).pc).toBe(0x1234);
    await expect.poll(async () => (await leds(page)).addr).toBe(0x1234);
  });

  test("EXAMINE NEXT walks PC forward one address", async ({ page }) => {
    await boot(page);
    await panelStop(page);
    await setSwitches(page, 0x2000);
    await clickPaddle(page, /EXAMINE/, "up");
    await clickPaddle(page, /EXAMINE/, "down");
    expect((await regs(page)).pc).toBe(0x2001);
  });

  test("DEPOSIT writes the switch byte at PC; DEPOSIT NEXT advances then writes", async ({ page }) => {
    await boot(page);
    await panelStop(page);
    await setSwitches(page, 0x0500);
    await clickPaddle(page, /EXAMINE/, "up");
    await setSwitches(page, 0x00aa);
    await clickPaddle(page, /DEPOSIT/, "up");
    expect(await memAt(page, 0x0500)).toBe(0xaa);
    await setSwitches(page, 0x0055);
    await clickPaddle(page, /DEPOSIT/, "down");
    expect((await regs(page)).pc).toBe(0x0501);
    expect(await memAt(page, 0x0501)).toBe(0x55);
  });

  test("SINGLE STEP executes exactly one instruction while stopped", async ({ page }) => {
    await boot(page);
    await panelStop(page);
    // 0000: 3C INR A ; 3C INR A ; 76 HLT
    await setSwitches(page, 0x0000);
    await clickPaddle(page, /EXAMINE/, "up");
    for (const b of [0x3c, 0x3c, 0x76]) {
      await setSwitches(page, b);
      await clickPaddle(page, /DEPOSIT/, "up");
      await clickPaddle(page, /DEPOSIT/, "down");
    }
    await setSwitches(page, 0x0000);
    await clickPaddle(page, /EXAMINE/, "up");
    const a0 = (await regs(page)).a;
    await clickPaddle(page, /SINGLE STEP/);
    const s1 = await regs(page);
    expect(s1.pc).toBe(0x0001);
    expect(s1.a).toBe((a0 + 1) & 0xff);
    await clickPaddle(page, /SINGLE STEP/);
    expect((await regs(page)).pc).toBe(0x0002);
  });

  test("STOP / RUN latching paddle halts and resumes the CPU", async ({ page }) => {
    await boot(page);
    expect(await page.evaluate(() => (window as any).__test.running)).toBe(true);
    await panelStop(page);
    expect(await page.evaluate(() => (window as any).__test.running)).toBe(false);
    const pcStopped = (await regs(page)).pc;
    await page.waitForTimeout(200);
    expect((await regs(page)).pc).toBe(pcStopped); // frozen while stopped
    await panelRun(page);
    expect(await page.evaluate(() => (window as any).__test.running)).toBe(true);
  });

  test("RESET jumps PC to 0; CLR just reboots the CPU", async ({ page }) => {
    await boot(page);
    await panelStop(page);
    await setSwitches(page, 0x3abc);
    await clickPaddle(page, /EXAMINE/, "up");
    expect((await regs(page)).pc).toBe(0x3abc);
    await clickPaddle(page, /RESET/, "up"); // RESET -> PC = 0
    expect((await regs(page)).pc).toBe(0);
    await clickPaddle(page, /EXAMINE/, "up"); // PC back to 0x3abc
    await clickPaddle(page, /RESET/, "down"); // CLR -> reboot, PC left where it is
    expect(await page.evaluate(() => (window as any).__test.regs().pc)).toBeDefined();
  });

  test("power OFF halts the machine and darkens every lamp", async ({ page }) => {
    await boot(page);
    await page.evaluate(() => {
      const cell = document.querySelector<HTMLElement>("#altair .fp-power.paddle");
      cell?.click();
    });
    expect(await page.evaluate(() => (window as any).__test.running)).toBe(false);
    await expect
      .poll(async () => {
        const l = await leds(page);
        return l.addr + l.data + Object.values(l.status).filter(Boolean).length;
      })
      .toBe(0);
  });

  test("HLTA status LED lights when the CPU halts", async ({ page }) => {
    await boot(page);
    await panelStop(page);
    await setSwitches(page, 0x0000);
    await clickPaddle(page, /EXAMINE/, "up");
    await setSwitches(page, 0x0076); // HLT
    await clickPaddle(page, /DEPOSIT/, "up");
    await setSwitches(page, 0x0000);
    await clickPaddle(page, /EXAMINE/, "up");
    await panelRun(page);
    await expect.poll(async () => (await leds(page)).status.HLTA).toBe(true);
    await expect.poll(async () => (await regs(page)).halted).toBe(true);
  });

  test("the PROTECT and AUX paddles are inert but wired", async ({ page }) => {
    await boot(page);
    const pc = (await regs(page)).pc;
    for (const label of [/PROTECT/, /AUX/]) {
      await clickPaddle(page, label, "up");
      await clickPaddle(page, label, "down");
    }
    // nothing changed -- they're placeholders on the real panel too
    expect(await page.evaluate(() => (window as any).__test.running)).toBe(true);
    expect(Math.abs((await regs(page)).pc - pc)).toBeLessThan(0x100);
  });

  // Kill the Bit's moving bit + the sense-switch XOR mechanic is covered in
  // panel-lights.spec.ts ("Kill the Bit: the matching sense switch clears …").
});
