import { test, expect } from "./fixtures";
import { boot } from "./helpers";

type Page = import("@playwright/test").Page;

const kHddImageBytes = 31900160;
const KEEN = "disk=keen1";

// FAT16 volume label in the first partition's boot sector
async function mountedVolumeLabel(page: Page): Promise<string> {
  return page.evaluate(() => {
    const img: Uint8Array = (window as any).__test.machine.hddImage();
    const lba = img[454] | (img[455] << 8) | (img[456] << 16) | (img[457] << 24);
    return String.fromCharCode(...img.subarray(lba * 512 + 43, lba * 512 + 54)).trim();
  });
}

test.describe("preset C: (?disk=keen1)", () => {
  test("the Keen image is a 30MB FAT16 C: with Keen in the root, loaded as the shell", async ({ page }) => {
    await boot(page, { params: KEEN, expectScreen: null });
    const r = await page.evaluate(async () => {
      const b = new Uint8Array(await (await fetch("disks/presets/keen1-hdd.img")).arrayBuffer());
      const dv = new DataView(b.buffer);
      const part = dv.getUint32(454, true) * 512;
      const bps = dv.getUint16(part + 11, true);
      const spc = b[part + 13];
      const rootEntries = dv.getUint16(part + 17, true);
      const root = part + (dv.getUint16(part + 14, true) + b[part + 16] * dv.getUint16(part + 22, true)) * bps;
      const data = root + rootEntries * 32;
      const files: Record<string, { cluster: number; size: number }> = {};
      for (let i = 0; i < rootEntries; i++) {
        const e = root + i * 32;
        if (b[e] === 0) break;
        if (b[e] === 0xe5 || b[e + 11] === 0x0f) continue;
        const name = String.fromCharCode(...b.subarray(e, e + 8)).trim();
        const ext = String.fromCharCode(...b.subarray(e + 8, e + 11)).trim();
        files[ext ? name + "." + ext : name] = { cluster: dv.getUint16(e + 26, true), size: dv.getUint32(e + 28, true) };
      }
      const cfg = files["CONFIG.SYS"];
      const at = data + (cfg.cluster - 2) * spc * bps;
      return {
        len: b.length,
        fs: String.fromCharCode(...b.subarray(part + 54, part + 62)),
        names: Object.keys(files),
        config: String.fromCharCode(...b.subarray(at, at + cfg.size)),
      };
    });
    expect(r.len).toBe(kHddImageBytes);
    expect(r.fs).toBe("FAT16   ");
    expect(r.names).toEqual(expect.arrayContaining(["KERNEL.SYS", "COMMAND.COM", "KEEN1.EXE", "EGAHEAD.CK1", "LEVEL01.CK1"]));
    expect(r.names).not.toContain("FREEDOS");
    expect(r.config).toMatch(/SHELL=C:\\KEEN1\.EXE\r\n$/);
  });

  test("mounts the Keen image and relabels the Hard disk panel", async ({ page }) => {
    await boot(page, { params: KEEN, expectScreen: null });
    expect(await mountedVolumeLabel(page)).toBe("KEEN");
    await expect(page.locator("#hddStatus")).toHaveText(/Using: Commander Keen 1 shareware/);
    await expect(page.locator("#hddResetBtn")).toHaveText("Reset to original Keen disk");
    await expect(page.locator("#hddBlankBtn")).toBeHidden();
    await expect(page.locator("#hddUploadBtn")).toBeHidden();
  });

  test("the visitor's own saved C: and the Keen C: never mount in each other's place", async ({ page }) => {
    await boot(page, { expectScreen: null });
    await page.evaluate(async () => {
      const img = new Uint8Array(await (await fetch("disks/freedos-hdd.img")).arrayBuffer());
      const lba = img[454] | (img[455] << 8) | (img[456] << 16) | (img[457] << 24);
      img.set(Array.from("MINE       ", (c) => c.charCodeAt(0)), lba * 512 + 43);
      const db: IDBDatabase = await new Promise((resolve, reject) => {
        const req = indexedDB.open("ibmpcat-hdd", 1);
        req.onupgradeneeded = () => req.result.createObjectStore("hdd");
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
      });
      await new Promise((resolve, reject) => {
        const tx = db.transaction("hdd", "readwrite");
        tx.objectStore("hdd").put(img, "c-drive");
        tx.oncomplete = resolve;
        tx.onerror = () => reject(tx.error);
      });
      db.close();
    });

    await boot(page, { params: KEEN, expectScreen: null });
    expect(await mountedVolumeLabel(page)).toBe("KEEN");

    await boot(page, { expectScreen: null });
    await expect(page.locator("#hddStatus")).toHaveText(/saved state \(from a previous visit\)/);
    expect(await mountedVolumeLabel(page)).toBe("MINE");
  });
});
