import { test, expect } from "./fixtures";
import {
  boot,
  waitForScreen,
  screen,
  setSwitches,
  clickPaddle,
  panelStop,
  panelRun,
  regs,
  hasCatalogItem,
  pickCatalogItem,
} from "./helpers";

test.describe("88-DCDD disk cabinet", () => {
  test("the cabinet has two drives and is shown on the CP/M preset", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await expect(page.locator("#dcdd")).not.toHaveClass(/empty/);
    expect(await page.locator("#dcdd .dcdd-bay").count()).toBe(2);
  });

  test("drive B: takes a second diskette and CP/M can read it", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    // boot A:
    await page.click("#dcdd .dcdd-boot");
    await waitForScreen(page, /A>/, 30_000);

    // load a diskette into drive B via its own slot dialog
    await page.click("#dcdd .dcdd-bay:nth-child(2) .dcdd-slot");
    await expect(page.locator("#diskDialog")).toBeVisible();
    await expect(page.locator("#diskDrive")).toHaveText("B");
    await pickCatalogItem(page, "#diskList", /CP\/M 2\.2/);
    await page.click("#diskInsert");
    await expect(page.locator("#dcdd .dcdd-bay").nth(1)).toHaveClass(/loaded/);
    expect(await page.evaluate(() => (window as any).__test.machine.diskPresent(1))).toBe(true);

    // CP/M sees it
    await page.evaluate(() => {
      const m = (window as any).__test.machine;
      for (const c of "DIR B:\r") m.sendByte(c.charCodeAt(0));
    });
    await waitForScreen(page, /B>|\bCOM\b|\.COM|A>\s*$/i, 20_000);
    expect(await screen(page)).not.toMatch(/NO FILE/i);
  });

  test("'Blank (Unformatted)' mounts a fresh, unformatted 8-inch image", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await page.click("#dcdd .dcdd-boot");
    await waitForScreen(page, /A>/, 30_000);

    await page.click("#dcdd .dcdd-bay:nth-child(2) .dcdd-slot");
    await expect(page.locator("#diskDialog")).toBeVisible();
    await expect(page.locator("#diskDialog")).toContainText(/FORMAT/); // tells you to format it
    await page.click("#diskBlank");
    await expect(page.locator("#diskDialog")).toBeHidden();
    await expect(page.locator("#dcdd .dcdd-bay").nth(1)).toHaveClass(/loaded/);

    const img = await page.evaluate(() => {
      const m = (window as any).__test.machine;
      const b = m.diskImage(1);
      return { len: b.length, e5: b[0] === 0xe5 && b[123456] === 0xe5, dirty: m.diskDirty(1) };
    });
    expect(img).toEqual({ len: 337568, e5: true, dirty: false });

    // real unformatted media: CP/M can't read it until FORMAT has run
    await page.evaluate(() => {
      const m = (window as any).__test.machine;
      for (const c of "DIR B:\r") m.sendByte(c.charCodeAt(0));
    });
    await waitForScreen(page, /Bdos Err|BAD SECTOR/i, 20_000);
  });

  test("'Blank (Formatted)' mounts a disk CP/M can use immediately", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await page.click("#dcdd .dcdd-boot");
    await waitForScreen(page, /A>/, 30_000);

    await page.click("#dcdd .dcdd-bay:nth-child(2) .dcdd-slot");
    await expect(page.locator("#diskDialog")).toBeVisible();
    await page.click("#diskBlankFmt");
    await expect(page.locator("#diskDialog")).toBeHidden();
    await expect(page.locator("#dcdd .dcdd-bay").nth(1)).toHaveClass(/loaded/);

    const img = await page.evaluate(() => {
      const m = (window as any).__test.machine;
      const b = m.diskImage(1);
      const dirBase = 2 * 32 * 137;   // track 2, sector 0 -- the CP/M directory
      return { len: b.length, dirEmpty: b[dirBase + 3] === 0xe5 && b[dirBase + 130] === 0xe5, dirty: m.diskDirty(1) };
    });
    expect(img).toEqual({ len: 337568, dirEmpty: true, dirty: false });

    // real MITS framing, empty directory: CP/M reads it right away as blank
    await page.evaluate(() => {
      const m = (window as any).__test.machine;
      for (const c of "DIR B:\r") m.sendByte(c.charCodeAt(0));
    });
    await waitForScreen(page, /NO FILE/i, 20_000);
  });

  test("BOOT brings up CP/M at the A> prompt", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await expect(page.locator("#dcdd .dcdd-bay").first()).toHaveClass(/loaded/);
    await page.click("#dcdd .dcdd-boot");
    await waitForScreen(page, /A>/, 30_000);
  });

  test("front-panel hand boot: switches 0FF00h / EXAMINE / RUN reaches A>", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await expect(page.locator("#dcdd .dcdd-bay").first()).toHaveClass(/loaded/);
    // the 88-DCDD boot PROM must be mapped at 0FF00h whenever the drive is fitted
    expect(await page.evaluate(() => (window as any).__test.machine.readByte(0xff00))).toBe(0x21);

    await panelStop(page);
    await setSwitches(page, 0xff00);
    await clickPaddle(page, /EXAMINE/, "up");
    expect((await regs(page)).pc).toBe(0xff00);
    await panelRun(page);
    await waitForScreen(page, /A>/, 30_000);
  });

  test("the boot PROM window is read-only even at 64 KB RAM", async ({ page }) => {
    await boot(page, { params: "preset=cpm" }); // 64 KB: ram_top_ alone would swallow 0xFF00
    expect(await page.evaluate(() => (window as any).__test.machine.readByte(0xff00))).toBe(0x21);

    await page.evaluate(() => {
      const m = (window as any).__test.machine;
      // MVI A,0AAh ; STA 0FF00h ; HLT -- runs through the CPU, so the write goes
      // through the bus (unlike writeByte(), which pokes mem_[] directly)
      m.loadBytes(new Uint8Array([0x3e, 0xaa, 0x32, 0x00, 0xff, 0x76]), 0x0000);
      m.setPC(0);
      for (let i = 0; i < 10 && !m.halted(); i++) m.stepOne();
    });
    expect(await page.evaluate(() => (window as any).__test.machine.halted())).toBe(true);
    expect(await page.evaluate(() => (window as any).__test.machine.readByte(0xff00))).toBe(0x21);
  });

  // ALTAIR_REVIEW.md §3.2b: RESET carries no STEP pulses, so a real drive's
  // head just stays put; it's only the controller that's a bus signal away.
  test("the front-panel RESET paddle does not move the disk head", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await page.click("#dcdd .dcdd-boot");
    await waitForScreen(page, /A>/, 30_000);
    const before = await page.evaluate(() => (window as any).__test.machine.diskStatus());
    expect(before.track0).toBeGreaterThan(0);   // the BIOS seeks away from track 0 while booting

    await panelStop(page);
    await clickPaddle(page, /RESET/, "up");
    const after = await page.evaluate(() => (window as any).__test.machine.diskStatus());
    expect(after.track0).toBe(before.track0);   // head untouched
    expect(after.selected).toBe(-1);            // but the controller is deselected
  });

  test("CP/M runs a command after booting", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await page.click("#dcdd .dcdd-boot");
    await waitForScreen(page, /A>/, 30_000);
    await page.evaluate(() => {
      const m = (window as any).__test.machine;
      for (const c of "DIR\r") m.sendByte(c.charCodeAt(0));
    });
    await waitForScreen(page, /A>\s*DIR|\.COM|CClP|COM\b/i, 20_000);
  });

  test("inserting a diskette through the drive dialog, then BOOT", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await page.evaluate(() => (window as any).__test.disk.eject(0));
    await expect(page.locator("#dcdd .dcdd-bay").first()).toHaveClass(/empty/);
    await page.click("#dcdd .dcdd-bay:first-child .dcdd-slot");
    await expect(page.locator("#diskDialog")).toBeVisible();
    await pickCatalogItem(page, "#diskList", /CP\/M 2\.2/);
    await page.click("#diskInsert");
    await expect(page.locator("#dcdd .dcdd-bay").first()).toHaveClass(/loaded/);
    await page.click("#dcdd .dcdd-boot");
    await waitForScreen(page, /A>/, 30_000);
  });

  test("the drive's ? button explains the mounted disk", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await page.click("#dcdd .dcdd-bay:first-child .dcdd-help");
    await expect(page.locator("#diskHelpDialog")).toBeVisible();
    await expect(page.locator("#diskHelpName")).toHaveText(/CP\/M/i);
    await expect(page.locator("#diskHelpBody")).toContainText(/USING CP\/M/);
    await page.click("#diskHelpOk");
    await expect(page.locator("#diskHelpDialog")).toBeHidden();
  });

  test("the drive activity lamp pulses during a boot read", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    const before = await page.evaluate(() => (window as any).__test.machine.diskStatus().track0);
    // watch the drive-A lamp element for the "busy" class while CP/M loads
    const watch = page.evaluate(async () => {
      const a = document.querySelector("#dcdd .dcdd-bay:first-child .dcdd-lamp")!;
      const b = document.querySelector("#dcdd .dcdd-bay:nth-child(2) .dcdd-lamp")!;
      let sawBusyA = false, sawBusyB = false;
      for (let i = 0; i < 400; i++) {
        if (a.classList.contains("busy") || a.classList.contains("sel")) sawBusyA = true;
        if (b.classList.contains("busy")) sawBusyB = true;
        await new Promise((r) => setTimeout(r, 15));
      }
      return { sawBusyA, sawBusyB };
    });
    await page.click("#dcdd .dcdd-boot");
    const { sawBusyA, sawBusyB } = await watch;
    expect(sawBusyA).toBe(true);
    expect(sawBusyB).toBe(false);   // the idle drive's lamp stays dark
    await waitForScreen(page, /A>/, 30_000);
    const after = await page.evaluate(() => (window as any).__test.machine.diskStatus().track0);
    expect(after).toBeGreaterThan(before);
  });

  test("EJECT empties the bay", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await page.evaluate(() => (window as any).__test.disk.eject(0));
    await expect(page.locator("#dcdd .dcdd-bay").first()).toHaveClass(/empty/);
  });

  test("the drive EJECT button empties the bay", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await page.click("#dcdd .dcdd-bay:first-child .dcdd-eject");
    await expect(page.locator("#dcdd .dcdd-bay").first()).toHaveClass(/empty/);
  });

  test("the disk-help dialog closes via its X", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await page.click("#dcdd .dcdd-bay:first-child .dcdd-help");
    await expect(page.locator("#diskHelpDialog")).toBeVisible();
    await page.click("#diskHelpClose");
    await expect(page.locator("#diskHelpDialog")).toBeHidden();
  });

  test("Choose File loads a local .dsk image", async ({ page }) => {
    const path = await import("path");
    await boot(page, { params: "preset=cpm" });
    await page.evaluate(() => (window as any).__test.disk.eject(0));
    await page.click("#dcdd .dcdd-bay:first-child .dcdd-slot");
    await page.setInputFiles(
      "#diskFile",
      path.join(__dirname, "..", "disks", "cpm63k.dsk"),
    );
    await page.click("#diskInsert");
    await expect(page.locator("#dcdd .dcdd-bay").first()).toHaveClass(/loaded/);
    await page.click("#dcdd .dcdd-boot");
    await waitForScreen(page, /A>/, 30_000);
  });

  test("swapping a diskette keeps the era preset (it's not a hardware change)", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    expect(await page.locator("#preset").inputValue()).toBe("cpm");
    await page.evaluate(() => (window as any).__test.disk.eject(0));
    await page.click("#dcdd .dcdd-bay:first-child .dcdd-slot");
    await pickCatalogItem(page, "#diskList", /CP\/M 2\.2/);
    await page.click("#diskInsert");
    expect(await page.locator("#preset").inputValue()).toBe("cpm");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.preset"))).toBe("cpm");
  });

  test("ejecting a disk with unsaved changes is refused when the confirm is dismissed", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await page.click("#dcdd .dcdd-boot");
    await waitForScreen(page, /A>/, 30_000);
    await page.evaluate(() => {
      const m = (window as any).__test.machine;
      for (const c of "SAVE 1 X.DAT\r") m.sendByte(c.charCodeAt(0));
    });
    await expect
      .poll(() => page.evaluate(() => (window as any).__test.machine.diskDirty(0)))
      .toBe(true);
    page.on("dialog", (d) => d.dismiss());
    await page.click("#dcdd .dcdd-bay:first-child .dcdd-eject");
    await expect(page.locator("#dcdd .dcdd-bay").first()).toHaveClass(/loaded/); // still in
  });

  test("a disk written by CP/M goes dirty and SAVE downloads it", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await page.click("#dcdd .dcdd-boot");
    await waitForScreen(page, /A>/, 30_000);
    await page.evaluate(() => {
      const m = (window as any).__test.machine;
      for (const c of "SAVE 1 TEST.DAT\r") m.sendByte(c.charCodeAt(0));
    });
    await expect(page.locator("#dcdd .dcdd-bay:first-child .dcdd-save")).toBeVisible({
      timeout: 15_000,
    });
    const dl = page.waitForEvent("download");
    await page.click("#dcdd .dcdd-bay:first-child .dcdd-save");
    const download = await dl;
    expect(download.suggestedFilename()).toMatch(/\.dsk$/);
    await expect(page.locator("#dcdd .dcdd-bay:first-child .dcdd-save")).toBeHidden();
  });

  for (const [label, re] of [
    ["CP/M Games", /CP\/M Games/],
    ["WordStar 3.0", /WordStar/],
    ["Zork I", /Zork/],
    ["Altair DOS 1.0", /Altair DOS/],
  ] as [string, RegExp][]) {
    test(`extra disk: ${label} (boots locally / marked unavailable in CI)`, async ({ page }) => {
      await boot(page, { params: "preset=cpm" });
      await page.evaluate(() => (window as any).__test.disk.eject(0));
      await page.click("#dcdd .dcdd-bay:first-child .dcdd-slot");
      await expect(page.locator("#diskDialog")).toBeVisible();
      const present = await hasCatalogItem(page, "#diskList", re);
      if (!present) {
        // CI: the image isn't fetched -> the option is there but disabled
        const disabled = await page.evaluate(
          ({ src, flags }) => {
            const r = new RegExp(src, flags);
            const sel = document.querySelector<HTMLSelectElement>("#diskList")!;
            return [...sel.options].some((o) => r.test(o.textContent || "") && o.disabled);
          },
          { src: re.source, flags: re.flags },
        );
        expect(disabled).toBe(true);
        return;
      }
      await pickCatalogItem(page, "#diskList", re);
      await page.click("#diskInsert");
      await expect(page.locator("#dcdd .dcdd-bay").first()).toHaveClass(/loaded/);
      await page.click("#dcdd .dcdd-boot");
      // the image mounts and boots far enough to write something to the console
      await expect
        .poll(() => screen(page).then((s) => s.replace(/\s/g, "").length), { timeout: 30_000 })
        .toBeGreaterThan(3);
    });
  }
});
