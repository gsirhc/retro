import { test, expect } from "./fixtures";

// Opt-in freeware media (CuteMouse floppy, FreeDOS CD) lives under
// Removable media: a DOS program reaches the mouse through INT 33h, which
// is a driver rather than firmware, so the machine ships the CuteMouse
// diskette rather than pretending the bare hardware is enough
// (PC486_REVIEW.md §10). The FreeDOS install/live CD is freely
// redistributable and fetched on demand, not at page load.
test.describe("Freeware Disks & Drivers", () => {
  test("sits in Removable media, after the hard disk panel", async ({ page }) => {
    await page.goto("/?test=1");
    const order = await page.evaluate(() => {
      const ids = ["frontPanelCard", "hddCard", "floppyInfoCard"];
      return ids.map((id) => {
        const el = document.getElementById(id);
        // Hidden panels (the Performance panel without ?perf) aren't on the page.
        return el ? Array.from(document.querySelectorAll(".panel:not([hidden])")).indexOf(el) : -1;
      });
    });
    expect(order[0]).toBeGreaterThanOrEqual(0);
    expect(order[1]).toBe(order[0] + 1);
    expect(order[2]).toBe(order[1] + 1);
    await expect(page.locator("#floppyInfoCard #ctmouseBtn")).toBeVisible();
    await expect(page.locator("#floppyInfoCard #freedosCdBtn")).toBeVisible();
  });

  test("Removable media hosts both opt-in media buttons", async ({
    page,
  }) => {
    await page.goto("/?test=1");
    await expect(page.locator("#floppyInfoCard h2")).toHaveText("Removable media");
    await expect(page.locator("#floppyInfoCard")).toContainText(
      "Useful freeware drivers and software"
    );
    await expect(page.locator("#ctmouseBtn")).toBeVisible();
    await expect(page.locator("#freedosCdBtn")).toHaveText(/Insert FreeDOS CD/);
    // The FreeDOS CD shortcut is not on the CD-ROM bay -- Insert/Eject stay there.
    await expect(
      page.locator('.at-bay[data-drive="cdrom"] #freedosCdBtn')
    ).toHaveCount(0);
  });

  test("inserting the CuteMouse disk loads a real 1.44MB image into drive A:", async ({
    livePage: page,
  }) => {
    const bay = page.locator('.at-bay[data-drive="0"]');
    await expect(bay).not.toHaveClass(/loaded/);

    const btn = page.locator("#ctmouseBtn");
    const status = page.locator("#ctmouseStatus");
    // The panel carries no standing commentary -- the how-to appears on use.
    await expect(status).toHaveText("");

    await btn.click();
    await expect(bay).toHaveClass(/loaded/, { timeout: 20_000 });
    await expect(bay.locator('[data-role="label"]')).toHaveText("ctmouse.img");
    // Ejecting must be possible, exactly as for a user-supplied diskette.
    await expect(bay.locator('[data-role="eject"]')).toBeEnabled();

    // The usage note has to cover loading it now AND making it stick, since a
    // TSR is gone at the next reboot.
    await expect(status).toContainText("CTMOUSE /P");
    await expect(status).toContainText("AUTOEXEC.BAT");
    await expect(status).toContainText("COPY CTMOUSE.EXE");

    // A real drive takes a diskette whenever you hand it one: the button never
    // latches off, and re-inserting over a loaded disk works.
    await expect(btn).toBeEnabled();
    await btn.click();
    await expect(bay).toHaveClass(/loaded/);
    await expect(btn).toBeEnabled();
  });

  test("the served image is a 1.44MB FAT12 diskette carrying CTMOUSE.EXE", async ({
    page,
  }) => {
    await page.goto("/?test=1");
    const info = await page.evaluate(async () => {
      const res = await fetch("disks/ctmouse.img");
      const buf = new Uint8Array(await res.arrayBuffer());
      // 8.3 names sit in the root directory, which for this geometry starts
      // after the boot sector and both FAT copies: 1 + 2*9 sectors.
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
