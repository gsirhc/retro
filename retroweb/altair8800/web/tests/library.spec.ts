import { test, expect } from "./fixtures";
import { boot, waitForScreen, screen, send, pickCatalogItem } from "./helpers";
import type { Page } from "@playwright/test";

async function insertFromLibrary(page: Page, drive: 0 | 1, name: RegExp): Promise<void> {
  const bay = page.locator("#dcdd .dcdd-bay").nth(drive);
  if (await bay.evaluate((el) => el.classList.contains("loaded")))
    await page.evaluate((d) => (window as any).__test.disk.eject(d), drive);
  await bay.locator(".dcdd-slot").click();
  await expect(page.locator("#diskDialog")).toBeVisible();
  await pickCatalogItem(page, "#diskList", name);
  await page.click("#diskInsert");
  await expect(bay).toHaveClass(/loaded/);
}

async function bootFromLibrary(page: Page, name: RegExp): Promise<void> {
  await boot(page, { params: "preset=cpm" });
  await insertFromLibrary(page, 0, name);
  await page.click("#dcdd .dcdd-boot");
}

async function answer(page: Page, prompt: RegExp, reply: string): Promise<void> {
  await waitForScreen(page, prompt, 30_000);
  await send(page, reply + "\r");
}

// a DataTransfer drop with a real File, the way a browser delivers a dragged file
async function dropFile(page: Page, name: string, bytes: number[]): Promise<void> {
  await page.evaluate(({ name, bytes }) => {
    const dt = new DataTransfer();
    dt.items.add(new File([new Uint8Array(bytes)], name));
    for (const type of ["dragenter", "dragover", "drop"])
      document.body.dispatchEvent(new DragEvent(type, { dataTransfer: dt, bubbles: true, cancelable: true }));
  }, { name, bytes });
}

test.describe("88-DCDD disk library", () => {
  test("every manifest entry is installed and listed with its year", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    const manifest = await page.evaluate(() => fetch("disks/manifest.json").then((r) => r.json()));
    await page.click("#dcdd .dcdd-bay:nth-child(2) .dcdd-slot");
    for (const d of manifest.disks) {
      const re = new RegExp(d.name.replace(/[.*+?^${}()|[\]\\/]/g, "\\$&"));
      await pickCatalogItem(page, "#diskList", re);
      const opt = page.locator("#diskList option:checked");
      await expect(opt).toBeEnabled();
      if (d.year) await expect(opt).toContainText(String(d.year));
    }
  });

  test("the picker shows year, publisher and what the disk needs", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await page.click("#dcdd .dcdd-bay:nth-child(2) .dcdd-slot");
    await pickCatalogItem(page, "#diskList", /WordStar/);
    await expect(page.locator("#diskMeta")).toHaveText(
      "1981  ·  MicroPro International  ·  Needs 64K RAM, drive A:, DEC VT52 terminal",
    );
    await expect(page.locator("#diskDesc")).toContainText("word processor");
  });

  test("opening the catalog fetches no disk image until one is inserted", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    const gets: string[] = [];
    page.on("request", (r) => {
      if (r.method() === "GET" && /\/disks\/.*\.dsk$/.test(r.url())) gets.push(r.url());
    });
    await page.click("#dcdd .dcdd-bay:nth-child(2) .dcdd-slot");
    await pickCatalogItem(page, "#diskList", /Zork/);
    expect(gets).toEqual([]);
    await page.click("#diskInsert");
    await expect.poll(() => gets.length).toBe(1);
    expect(gets[0]).toMatch(/zork\.dsk$/);
  });

  test("the ? dialog shows the license and the Disk BASIC primer", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await insertFromLibrary(page, 0, /Disk Extended BASIC/);
    await page.click("#dcdd .dcdd-bay:first-child .dcdd-help");
    const body = page.locator("#diskHelpBody");
    await expect(body).toContainText("1979  ·  MITS / Microsoft");
    await expect(body).toContainText("Altair BASIC tapes");
    await expect(body).toContainText("USING DISK BASIC");
    await expect(body).toContainText('KILL "NAME"');
  });

  test("Disk BASIC boots, cold-starts and runs a program from the disk in B:", async ({ page }) => {
    await bootFromLibrary(page, /Disk Extended BASIC/);
    await insertFromLibrary(page, 1, /Disk BASIC Programs/);
    await answer(page, /MEMORY SIZE\?\s*$/, "");
    await answer(page, /LINEPRINTER\?\s*$/, "C");
    await answer(page, /HIGHEST DISK NUMBER\?\s*$/, "1");
    await answer(page, /HOW MANY FILES\?\s*$/, "3");
    await answer(page, /HOW MANY RANDOM FILES\?\s*$/, "2");
    await answer(page, /BYTES FREE\s+OK\s*$/, "MOUNT 0,1");
    await answer(page, /MOUNT 0,1\s+OK\s*$/, "FILES 1");
    await waitForScreen(page, /STARTREK[\s\S]*WUMPUS/, 30_000);
    await answer(page, /FILES 1[\s\S]*OK\s*$/, 'RUN "HELLO",1');
    await waitForScreen(page, /MY NAME IS MITS/, 30_000);
  });

  test("Disk BASIC's KILL and UNLOAD from the primer are real commands", async ({ page }) => {
    await bootFromLibrary(page, /Disk Extended BASIC/);
    await answer(page, /MEMORY SIZE\?\s*$/, "");
    await answer(page, /LINEPRINTER\?\s*$/, "C");
    await answer(page, /HIGHEST DISK NUMBER\?\s*$/, "0");
    await answer(page, /HOW MANY FILES\?\s*$/, "3");
    await answer(page, /HOW MANY RANDOM FILES\?\s*$/, "2");
    await answer(page, /BYTES FREE\s+OK\s*$/, "MOUNT 0");
    await answer(page, /MOUNT 0\s+OK\s*$/, '10 PRINT "HI"');
    await send(page, 'SAVE "ZZTEST"\r');
    await answer(page, /SAVE "ZZTEST"\s+OK\s*$/, 'KILL "ZZTEST"');
    await answer(page, /KILL "ZZTEST"\s+OK\s*$/, "UNLOAD");
    await waitForScreen(page, /UNLOAD\s+OK\s*$/, 30_000);
    expect(await screen(page)).not.toMatch(/SYNTAX ERROR|SN ERROR/i);
  });

  test("WordStar comes up on a VT52", async ({ page }) => {
    await bootFromLibrary(page, /WordStar/);
    await page.selectOption("#termProfile", "vt52");
    await answer(page, /A>\s*$/, "WS");
    await waitForScreen(page, /N O - F I L E\s+M E N U/, 60_000);
  });

  test("SuperCalc2 starts", async ({ page }) => {
    await bootFromLibrary(page, /SuperCalc2/);
    await page.selectOption("#termProfile", "vt100g");
    await answer(page, /A>\s*$/, "SC2");
    await waitForScreen(page, /SuperCalc2[\s\S]*SORCIM/, 60_000);
  });

  test("Zork I starts West of House", async ({ page }) => {
    await bootFromLibrary(page, /Zork/);
    await answer(page, /A>\s*$/, "ZORK1");
    await waitForScreen(page, /Revision 88 \/ Serial number 840726[\s\S]*West of House/, 60_000);
  });

  test("CP/M 3.0 boots from either disk", async ({ page }) => {
    await bootFromLibrary(page, /CP\/M 3\.0 \(boot/);
    await waitForScreen(page, /CP\/M 3 BIOS[\s\S]*A>\s*$/, 60_000);
    await answer(page, /A>\s*$/, "DIR");
    await waitForScreen(page, /MBASIC\s+COM/, 30_000);

    await insertFromLibrary(page, 0, /CP\/M 3\.0 \(util/);
    await page.click("#dcdd .dcdd-boot");
    await waitForScreen(page, /CP\/M 3 BIOS[\s\S]*A>\s*$/, 60_000);
    await answer(page, /A>\s*$/, "DIR");
    await waitForScreen(page, /GENCOM\s+COM/, 30_000);
  });

  test("a dropped .dsk goes to A:, then B: once A: is full", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await page.evaluate(() => (window as any).__test.disk.eject(0));
    const img = Array.from({ length: 337568 }, () => 0xe5);
    await dropFile(page, "mine.dsk", img);
    await expect(page.locator("#dcdd .dcdd-bay").nth(0)).toHaveClass(/loaded/);
    await expect(page.locator("#dcdd .dcdd-bay").nth(0).locator(".dcdd-slot")).toHaveAttribute("data-label", "MINE.DSK");
    await dropFile(page, "other.dsk", img);
    await expect(page.locator("#dcdd .dcdd-bay").nth(1)).toHaveClass(/loaded/);
    expect(await page.evaluate(() => (window as any).__test.machine.diskPresent(1))).toBe(true);
  });

  test("a drop over the page shows where the disk will land", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await expect(page.locator("#dcdd .dcdd-bay").nth(0)).toHaveClass(/loaded/);
    await page.evaluate(() => {
      const dt = new DataTransfer();
      dt.items.add(new File([new Uint8Array(4)], "x.dsk"));
      document.body.dispatchEvent(new DragEvent("dragenter", { dataTransfer: dt, bubbles: true, cancelable: true }));
    });
    await expect(page.locator(".disk-drop-veil")).toBeVisible();
    await expect(page.locator(".disk-drop-msg")).toHaveText("Drop a .dsk image to put it in drive B:");
    await page.evaluate(() => {
      const dt = new DataTransfer();
      dt.items.add(new File([new Uint8Array(4)], "x.dsk"));
      document.body.dispatchEvent(new DragEvent("dragleave", { dataTransfer: dt, bubbles: true }));
    });
    await expect(page.locator(".disk-drop-veil")).toBeHidden();
  });

  test("a dropped file that isn't a .dsk is refused", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await page.evaluate(() => (window as any).__test.disk.eject(0));
    await dropFile(page, "notes.txt", [1, 2, 3]);
    await expect(page.locator("#presetNote")).toContainText("notes.txt isn't a .dsk image");
    await expect(page.locator("#dcdd .dcdd-bay").nth(0)).toHaveClass(/empty/);
  });

  test("a drop with no 88-DCDD fitted says how to fit one", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await dropFile(page, "mine.dsk", [0xe5]);
    await expect(page.locator("#presetNote")).toContainText("No 88-DCDD here");
    expect(await page.evaluate(() => (window as any).__test.machine.diskPresent(0))).toBe(false);
  });
});
