import { test, expect } from "./fixtures";
import { boot, bay, typeStr, focusScreen, waitForScreen } from "./helpers";
import type { Page } from "@playwright/test";

async function openLibrary(page: Page) {
  await page.locator("#libraryBtn").click();
  await expect(page.locator("#libraryDialog")).toBeVisible();
  await expect(page.locator("#libraryDialog .lib-item").first()).toBeVisible();
}

async function dropFile(page: Page, name: string, size: number) {
  await page.evaluate(({ name, size }) => {
    const dt = new DataTransfer();
    dt.items.add(new File([new Uint8Array(size)], name));
    for (const type of ["dragenter", "dragover", "drop"])
      document.body.dispatchEvent(new DragEvent(type, { dataTransfer: dt, bubbles: true, cancelable: true }));
  }, { name, size });
}

test.describe("Disk library", () => {
  test("every image in library.json is served as a FAT12 floppy of its format", async ({ page }) => {
    await boot(page, { expectScreen: null });
    const report = await page.evaluate(async () => {
      const lib = await (await fetch("disks/library/library.json")).json();
      const out: { file: string; len: number; k: number; sig: boolean; sectors: number }[] = [];
      for (const e of lib.disks) {
        for (const img of e.images) {
          const b = new Uint8Array(await (await fetch("disks/library/" + img.file)).arrayBuffer());
          out.push({
            file: img.file,
            len: b.length,
            k: e.build ? e.build.format : e.format,
            sig: b[510] === 0x55 && b[511] === 0xaa,
            sectors: b[19] | (b[20] << 8),
          });
        }
      }
      return out;
    });
    expect(report.length).toBeGreaterThan(0);
    for (const r of report) {
      expect(r.len, r.file).toBe(r.k * 1024);
      expect(r.sectors, r.file).toBe(r.len / 512);
      expect(r.sig, r.file).toBe(true);
    }
  });

  test("lists each title with its year and shows what it needs", async ({ page }) => {
    await boot(page, { expectScreen: null });
    await openLibrary(page);
    const lib = await page.evaluate(() => fetch("disks/library/library.json").then((r) => r.json()));
    await expect(page.locator("#libraryDialog .lib-item")).toHaveCount(lib.disks.length);
    await page.locator("#libraryDialog .lib-item", { hasText: "Commander Keen" }).click();
    await expect(page.locator("#libraryDialog .lib-meta")).toHaveText("1990  ·  id Software / Apogee  ·  Needs EGA, hard disk");
    await expect(page.locator("#libraryDialog .lib-help")).toContainText("KEEN1");
    await page.locator("#libraryDialog .lib-close").click();
    await expect(page.locator("#libraryDialog")).toBeHidden();
  });

  test("a 360K disk offers both drives, a 1.2MB disk only A:", async ({ page }) => {
    await boot(page, { expectScreen: null });
    await openLibrary(page);
    await page.locator("#libraryDialog .lib-item", { hasText: "Commander Keen" }).click();
    await expect(page.locator("#libraryDialog .lib-disks button")).toHaveText(["Insert in A:", "Insert in B:"]);
    await page.locator("#libraryDialog .lib-item", { hasText: "FreeDOS Games" }).click();
    await expect(page.locator("#libraryDialog .lib-disks button")).toHaveText(["Insert in A:"]);
  });

  test("inserting from the library mounts the image and labels the bay", async ({ page }) => {
    await boot(page, { expectScreen: null });
    await openLibrary(page);
    await page.locator("#libraryDialog .lib-item", { hasText: "FreeDOS Editors and Tools" }).click();
    await page.locator('#libraryDialog .lib-disks button[data-file="fdtools.img"]').click();
    await expect(page.locator("#libraryDialog .lib-status")).toHaveText("It is in A:");
    await expect(bay(page, 0)).toHaveClass(/loaded/);
    await expect(bay(page, 0).locator('[data-role="label"]')).toHaveText("FreeDOS Editors and Tools");
    expect(await page.evaluate(() => (window as any).__test.machine.floppyImage(0).length)).toBe(1228800);
  });

  test("SAVE shows once DOS writes to the disk, and downloads it", async ({ page }) => {
    await boot(page);
    await openLibrary(page);
    await page.locator("#libraryDialog .lib-item", { hasText: "FreeDOS Games" }).click();
    await page.locator("#libraryDialog .lib-disks button").first().click();
    await expect(bay(page, 0)).toHaveClass(/loaded/);
    await page.locator("#libraryDialog .lib-close").click();
    const save = bay(page, 0).locator('[data-role="save"]');
    await expect(save).toBeHidden();
    await focusScreen(page);
    await waitForScreen(page, /C:\\>\s*$/);
    await typeStr(page, "COPY C:\\FDCONFIG.SYS A:");
    await waitForScreen(page, /=>> a:fdconfig\.sys/i, 60_000);
    await expect(save).toBeVisible();
    const dl = page.waitForEvent("download");
    await save.click();
    expect((await dl).suggestedFilename()).toBe("FreeDOS_Games.img");
    await expect(save).toBeHidden();
  });

  test("a dropped image goes to the first empty drive that takes it", async ({ page }) => {
    await boot(page, { expectScreen: null });
    await dropFile(page, "big.img", 1228800);
    await expect(bay(page, 0)).toHaveClass(/loaded/);
    await dropFile(page, "small.img", 368640);
    await expect(bay(page, 1)).toHaveClass(/loaded/);
    await expect(bay(page, 1).locator('[data-role="label"]')).toHaveText("small.img");
  });

  test("a dropped 1.2MB image never lands in the 360K drive", async ({ page }) => {
    await boot(page, { expectScreen: null });
    await dropFile(page, "one.img", 1228800);
    await dropFile(page, "two.img", 1228800);
    await expect(bay(page, 0).locator('[data-role="label"]')).toHaveText("two.img");
    await expect(bay(page, 1)).not.toHaveClass(/loaded/);
  });

  test("a drop that isn't a floppy image is refused with a reason", async ({ page }) => {
    await boot(page, { expectScreen: null });
    await dropFile(page, "photo.jpg", 100);
    await expect(page.locator(".disk-drop-msg")).toHaveText("photo.jpg isn't a floppy image. The drives take .img files.");
    await dropFile(page, "odd.img", 5000);
    await expect(page.locator(".disk-drop-msg")).toHaveText("odd.img isn't a 1.2MB or 360K floppy image.");
    await expect(bay(page, 0)).not.toHaveClass(/loaded/);
  });
});
