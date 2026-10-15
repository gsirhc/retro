import { test, expect } from "./fixtures";

// The CuteMouse floppy lives in the Disk library. INT 33h is a
// driver, not firmware, so the machine ships the diskette (PC486_REVIEW.md §10).
test.describe("Freeware disks & drivers", () => {
  test("Removable media has the Disk library and no separate media buttons", async ({ page }) => {
    await page.goto("/?test=1");
    await expect(page.locator("#floppyInfoCard h2")).toHaveText("Removable media");
    await expect(page.locator("#floppyInfoCard #libraryBtn")).toBeVisible();
    await expect(page.locator("#ctmouseBtn")).toHaveCount(0);
    await expect(page.locator("#freedosCdBtn")).toHaveCount(0);
  });

  test("the library offers CuteMouse for A:", async ({ page }) => {
    await page.goto("/?test=1");
    await page.locator("#libraryBtn").click();
    await page.locator("#libraryDialog .lib-item", { hasText: "CuteMouse" }).click();
    await expect(page.locator("#libraryDialog .lib-disks button")).toHaveText(["Insert in A:"]);
    await expect(page.locator("#libraryDialog .lib-help")).toContainText("CTMOUSE /P");
  });

  test("inserting the CuteMouse disk loads a real 1.44MB image into drive A:", async ({ livePage: page }) => {
    const bay = page.locator('.at-bay[data-drive="0"]');
    await expect(bay).not.toHaveClass(/loaded/);
    await page.locator("#libraryBtn").click();
    await page.locator("#libraryDialog .lib-item", { hasText: "CuteMouse" }).click();
    await page.locator("#libraryDialog .lib-disks button").click();
    await expect(bay).toHaveClass(/loaded/, { timeout: 20_000 });
    await expect(bay.locator('[data-role="label"]')).toHaveText("CuteMouse 2.1 driver");
    await expect(bay.locator('[data-role="eject"]')).toBeEnabled();
    expect(await page.evaluate(() => (window as any).__test.machine.floppyImage().length)).toBe(1474560);
    await page.locator("#libraryDialog .lib-close").click();
  });

  test("the served image is a 1.44MB FAT12 diskette carrying CTMOUSE.EXE", async ({ page }) => {
    await page.goto("/?test=1");
    const info = await page.evaluate(async () => {
      const res = await fetch("disks/library/ctmouse.img");
      const buf = new Uint8Array(await res.arrayBuffer());
      // 8.3 names sit in the root directory, after the boot sector and both FATs: 1 + 2*9 sectors.
      const root = new TextDecoder("latin1").decode(buf.subarray(19 * 512, 19 * 512 + 512));
      return { ok: res.ok, len: buf.length, root };
    });
    expect(info.ok).toBe(true);
    expect(info.len).toBe(1474560);
    expect(info.root).toContain("CTMOUSE EXE");
    expect(info.root).toContain("MOUSETST");
    expect(info.root).toContain("README  TXT");
  });
});
