import { test, expect } from "./fixtures";
import { boot, screen, waitForScreen, send, stubSavePicker } from "./helpers";

test.describe("error & fallback paths", () => {
  test("a missing roms/manifest.json leaves an empty but working reader", async ({ page }) => {
    await page.route("**/roms/manifest.json", (r) => r.abort());
    await boot(page, { params: "preset=stock" });
    await page.click("#ptr .ptr-window");
    await expect(page.locator("#ptrDialog")).toBeVisible();
    // no catalog, but Choose File still there and the machine still runs
    await send(page, "HI");
    await expect.poll(() => screen(page)).toMatch(/HI/);
  });

  test("a missing disks/manifest.json shows the not-found option", async ({ page }) => {
    await page.route("**/disks/manifest.json", (r) => r.abort());
    await boot(page, { params: "preset=cpm" });
    await page.click("#dcdd .dcdd-bay:nth-child(2) .dcdd-slot");
    await expect(page.locator("#diskList")).toContainText(/not found/i);
  });

  test("a missing tapes/manifest.json shows the no-library option", async ({ page }) => {
    await page.route("**/tapes/manifest.json", (r) => r.abort());
    await boot(page, { params: "preset=cassette" });
    await page.evaluate(() => (window as any).__test.cassette.eject(true));
    await page.click("#acr .acr-window");
    await expect(page.locator("#tapeList")).toContainText(/no cassette library/i);
  });

  test("a tape image that fails to fetch is listed as unavailable", async ({ page }) => {
    await page.route(/\/tapes\/.*\.cas$/i, (r) => r.abort());
    await boot(page, { params: "preset=cassette" });
    await page.evaluate(() => (window as any).__test.cassette.eject(true));
    await page.click("#acr .acr-window");
    await expect(page.locator("#tapeList")).toContainText(/unavailable/i);
  });

  test("a ROM image whose fetch errors is listed as unavailable", async ({ page }) => {
    await page.route("**/roms/killbits.bin", (r) => r.abort());
    await boot(page, { params: "preset=stock" });
    await page.click("#ptr .ptr-window");
    await expect(
      page.locator("#ptrList option", { hasText: /Kill the Bit/ }),
    ).toContainText(/unavailable/i);
  });

  test("a preset whose disk image 404s configures the drive and says so", async ({ page }) => {
    await page.route(/\/disks\/.*\.dsk$/i, (r) => r.abort());
    await page.goto("/?test=1&preset=cpm");
    await page.waitForFunction(() => (window as any).__test?.applyingPreset === false);
    await expect(page.locator("#presetNote")).toContainText(/could not be loaded/i, { timeout: 10_000 });
    await page.check("#autoload");
    await waitForScreen(page, /didn't load|could not be loaded/i, 10_000);
    await expect(page.locator("#dcdd")).not.toHaveClass(/empty/); // hardware still fitted
  });

  test("BOOT with an empty drive A flashes a hint", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await page.evaluate(() => (window as any).__test.disk.eject(0));
    await page.click("#dcdd .dcdd-boot");
    await expect(page.locator("#dcdd .dcdd-hint")).toContainText(/insert a diskette in drive A/i);
  });

  test("the panel guide's Boot button, with no disk, flashes a hint", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await page.evaluate(() => (window as any).__test.disk.eject(0));
    await page.click("#pgToggle");
    await page.click("#pgBoot");
    await expect(page.locator("#panelGuide")).toContainText(/insert a diskette in drive A/i);
  });

  test("the panel guide's Feed button, with no tape, flashes a hint", async ({ page }) => {
    await boot(page);
    await page.selectOption("#preset", "");
    await page.check('#deviceChips input[data-dev="papertape"]');
    await page.click("#pgToggle");
    await page.click("#pgFeed");
    await expect(page.locator("#panelGuide")).toContainText(/thread a tape/i);
  });

  test("the reader's own flash line shows a hint when LOAD has nothing threaded", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await page.evaluate(() => (window as any).__test.paperTape.load());
    await expect(page.locator("#ptr .ptr-hint")).toContainText(/thread a tape/i);
  });

  test("START with nothing threaded flashes a hint and does nothing", async ({ page }) => {
    await boot(page);
    await page.selectOption("#preset", "");
    await page.check('#deviceChips input[data-dev="papertape"]'); // reader fitted, empty
    const ok = await page.evaluate(() => (window as any).__test.paperTape.startReader());
    expect(ok).toBe(false);
    await expect(page.locator("#ptr .ptr-hint")).toContainText(/thread a tape/i);
    expect(await page.evaluate(() => (window as any).__test.tape.phase)).toBe("idle");
  });

  test("stopping the reader when it isn't running is a no-op", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await page.evaluate(() => (window as any).__test.tape.stop());
    expect(await page.evaluate(() => (window as any).__test.tape.phase)).toBe("idle");
    await expect(page.locator("#ptr")).not.toHaveClass(/reading/);
  });

  test("a wrong-size .dsk still mounts (with a console warning)", async ({ page }) => {
    const path = await import("path");
    const warnings: string[] = [];
    page.on("console", (m) => m.type() === "warning" && warnings.push(m.text()));
    await boot(page, { params: "preset=cpm" });
    await page.evaluate(() => (window as any).__test.disk.eject(0));
    await page.click("#dcdd .dcdd-bay:first-child .dcdd-slot");
    await page.setInputFiles("#diskFile", path.join(__dirname, "..", "roms", "hello.bin"));
    await page.click("#diskInsert");
    await expect(page.locator("#dcdd .dcdd-bay").first()).toHaveClass(/loaded/);
    expect(warnings.join("\n")).toMatch(/337568/);
  });

  test("ejecting a dirty tape is refused when the confirm is dismissed", async ({ page }) => {
    await boot(page, { params: "preset=cassette" });
    // make the tape dirty: blank + REC + CSAVE from BASIC
    await page.check("#autoload");
    await waitForScreen(page, /\bOK\b/, 40_000);
    await page.evaluate(() => (window as any).__test.cassette.eject(true));
    await page.evaluate(() => (window as any).__test.cassette.blank());
    await page.click("#acr .acr-key.rec");
    await send(page, 'CSAVE "T"\r');
    await expect.poll(() => page.evaluate(() => (window as any).__test.machine.tapeDirty())).toBe(true);
    page.on("dialog", (d) => d.dismiss()); // "eject anyway?" -> No
    await page.click("#acr .acr-key.ej");
    await expect(page.locator("#acr")).toHaveClass(/loaded/); // still in
  });

  test("cassette SAVE falls back to a download when the picker is unavailable", async ({ page }) => {
    await page.addInitScript(() => {
      delete (window as any).showSaveFilePicker;
    });
    await boot(page, { params: "preset=cassette" });
    await page.check("#autoload");
    await waitForScreen(page, /\bOK\b/, 40_000);
    await page.evaluate(() => (window as any).__test.cassette.eject(true));
    await page.evaluate(() => (window as any).__test.cassette.blank());
    await page.click("#acr .acr-key.rec");
    await send(page, '10 PRINT 1\rCSAVE "T"\r');
    await expect(page.locator("#acr .acr-save")).toBeVisible({ timeout: 15_000 });
    const dl = page.waitForEvent("download");
    await page.click("#acr .acr-save");
    expect((await dl).suggestedFilename()).toMatch(/\.cas$/);
  });

  test("cassette SAVE is a no-op if the user cancels the picker (AbortError)", async ({ page }) => {
    await page.addInitScript(() => {
      (window as any).showSaveFilePicker = async () => {
        const e = new Error("cancelled");
        e.name = "AbortError";
        throw e;
      };
    });
    await boot(page, { params: "preset=cassette" });
    await page.check("#autoload");
    await waitForScreen(page, /\bOK\b/, 40_000);
    await page.evaluate(() => (window as any).__test.cassette.eject(true));
    await page.evaluate(() => (window as any).__test.cassette.blank());
    await page.click("#acr .acr-key.rec");
    await send(page, '10 PRINT 1\rCSAVE "T"\r');
    await expect(page.locator("#acr .acr-save")).toBeVisible({ timeout: 15_000 });
    await page.click("#acr .acr-save");
    // dirty flag stays set (nothing was written); button still shown
    await page.waitForTimeout(300);
    expect(await page.evaluate(() => (window as any).__test.machine.tapeDirty())).toBe(true);
  });

  test("the tape dialog's Choose File updates the filename label", async ({ page }) => {
    const path = await import("path");
    await boot(page, { params: "preset=cassette" });
    await page.click("#acr .acr-window");
    await page.setInputFiles("#tapeFile", path.join(__dirname, "..", "tapes", "startrek.cas"));
    await expect(page.locator("#tapeFileName")).toHaveText(/startrek\.cas/);
    await page.click("#tapeInsert");
    await expect(page.locator("#acr")).toHaveClass(/loaded/);
  });

  test("every ?debug getter responds", async ({ page }) => {
    // seed a custom build with load devices so __dbg.devices has entries to map
    await page.goto("/?debug");
    await page.waitForFunction(() => !!(window as any).__dbg);
    await page.evaluate(() =>
      localStorage.setItem("retro8080.devices", JSON.stringify(["papertape", "disk"])),
    );
    await page.goto("/?debug");
    await page.waitForFunction(() => !!(window as any).__dbg);
    await page.selectOption("#preset", ""); // custom build -> chips visible & checked
    await expect(page.locator('#deviceChips input[data-dev="disk"]')).toBeChecked();
    const d = await page.evaluate(() => {
      const g = (window as any).__dbg;
      const l = (window as any).__loader;
      return {
        screen: typeof g.screen,
        tape: g.tape,
        spin: typeof g.spin,
        reading: typeof g.reading,
        preset: g.preset,
        devices: g.devices,
        ram: g.ram,
        pc: g.pc,
        running: g.running,
        mem: g.mem(0),
        rx: typeof l.rx,
        pending: l.pending,
      };
    });
    expect(d.screen).toBe("string");
    expect(d.preset).toBe("");
    expect(d.devices).toEqual(expect.arrayContaining(["papertape", "disk"]));
    expect(typeof d.ram).toBe("number");
    expect(typeof d.pc).toBe("number");
    expect(typeof d.running).toBe("boolean");
    expect(typeof d.mem).toBe("number");
    expect(d.rx).toBe("string");
  });

  test("every load dialog closes via its X and its Cancel button", async ({ page }) => {
    await boot(page);
    await page.selectOption("#preset", "");
    for (const d of ["papertape", "cassette", "disk"]) {
      await page.check(`#deviceChips input[data-dev="${d}"]`);
    }
    // paper-tape reader
    await page.click("#ptr .ptr-window");
    await expect(page.locator("#ptrDialog")).toBeVisible();
    await page.click("#ptrClose");
    await expect(page.locator("#ptrDialog")).toBeHidden();
    await page.click("#ptr .ptr-window");
    await page.click("#ptrCancel");
    await expect(page.locator("#ptrDialog")).toBeHidden();
    // cassette
    await page.click("#acr .acr-window");
    await page.click("#tapeClose");
    await expect(page.locator("#tapeDialog")).toBeHidden();
    await page.click("#acr .acr-window");
    await page.click("#tapeCancel");
    await expect(page.locator("#tapeDialog")).toBeHidden();
    // disk
    await page.click("#dcdd .dcdd-bay:first-child .dcdd-slot");
    await page.click("#diskClose");
    await expect(page.locator("#diskDialog")).toBeHidden();
    await page.click("#dcdd .dcdd-bay:first-child .dcdd-slot");
    await page.click("#diskCancel");
    await expect(page.locator("#diskDialog")).toBeHidden();
  });

  test("every ?test getter responds", async ({ page }) => {
    await boot(page, { params: "preset=cassette" });
    const t = await page.evaluate(() => {
      const x = (window as any).__test;
      return {
        switchWord: x.switchWord,
        loaderToken: x.loaderToken,
        applyingPreset: x.applyingPreset,
        outQLen: x.outQLen,
        rxWatch: typeof x.rxWatch,
        running: x.running,
        regs: typeof x.regs().pc,
        leds: typeof x.leds().addr,
        screen: typeof x.screen(),
      };
    });
    expect(t.switchWord).toBe(0);
    expect(typeof t.loaderToken).toBe("number");
    expect(t.applyingPreset).toBe(false);
    expect(typeof t.outQLen).toBe("number");
    expect(t.rxWatch).toBe("string");
    expect(t.running).toBe(true);
    expect(t.regs).toBe("number");
    expect(t.leds).toBe("number");
    expect(t.screen).toBe("string");
  });
});
