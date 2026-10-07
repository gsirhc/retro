import { test, expect } from "./fixtures";
import { boot, bootLive, waitForScreen, setPowerSwitch, typeStr, focusScreen, expectDownload } from "./helpers";

// C: controls. Reset/blank/download/upload take effect on next power-on; buttons are disabled
// while running and upload/reset/blank until firmware loads. State persists in IndexedDB via
// hdd-worker.js, and a C: saved by the 504MB builds converts on load.

type Page = import("@playwright/test").Page;

const kHddImageBytes = 255974400;

/** Mark C: as the visitor's own even if FreeDOS never dirtied it this boot. */
async function forcePersistHdd(page: Page): Promise<void> {
  await page.evaluate(async () => {
    await (window as any).__test.forcePersistHdd();
    await (window as any).__test.whenHddSaved();
  });
  await expect(page.locator("#hddStatus")).toHaveText(/saved \(this session\)/);
}

// Mirrors hdd-worker.js's database; the only seam to inspect or seed it.
const HDD_DB_NAME = "pc486-hdd";
const HDD_STORE = "hdd";
const META_KEY = "c2-meta";
const LEGACY_KEY = "c-drive";

async function idbGet(page: Page, key: string): Promise<any> {
  return page.evaluate(
    async ({ dbName, store, key }) => {
      const db: IDBDatabase = await new Promise((resolve, reject) => {
        const req = indexedDB.open(dbName, 1);
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
      });
      const val = await new Promise((resolve, reject) => {
        const tx = db.transaction(store, "readonly");
        const req = tx.objectStore(store).get(key);
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
      });
      db.close();
      if (val instanceof ArrayBuffer) return { arrayBuffer: val.byteLength };
      if (val instanceof Blob) return { blob: val.size };
      return val;
    },
    { dbName: HDD_DB_NAME, store: HDD_STORE, key },
  );
}

async function idbPutMeta(page: Page, patch: Record<string, unknown>): Promise<void> {
  await page.evaluate(
    async ({ dbName, store, key, patch }) => {
      const db: IDBDatabase = await new Promise((resolve, reject) => {
        const req = indexedDB.open(dbName, 1);
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
      });
      await new Promise<void>((resolve, reject) => {
        const tx = db.transaction(store, "readwrite");
        const s = tx.objectStore(store);
        const req = s.get(key);
        req.onsuccess = () => s.put({ ...req.result, ...patch }, key);
        tx.oncomplete = () => resolve();
        tx.onerror = () => reject(tx.error);
      });
      db.close();
    },
    { dbName: HDD_DB_NAME, store: HDD_STORE, key: META_KEY, patch },
  );
}

// Replaces the stored C: with a 504MB build's save: the gzip Blob under "c-drive", kept gzipped.
async function seedLegacySave(page: Page, fixture: string): Promise<void> {
  await page.evaluate(
    async ({ dbName, store, url }) => {
      const blob = await (await fetch(url)).blob();
      const db: IDBDatabase = await new Promise((resolve, reject) => {
        const req = indexedDB.open(dbName, 1);
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
      });
      await new Promise<void>((resolve, reject) => {
        const tx = db.transaction(store, "readwrite");
        const s = tx.objectStore(store);
        s.clear();
        s.put(blob, "c-drive");
        tx.oncomplete = () => resolve();
        tx.onerror = () => reject(tx.error);
      });
      db.close();
    },
    { dbName: HDD_DB_NAME, store: HDD_STORE, url: `tests/media/${fixture}.img.gz` },
  );
}

/** Powered off with every pending save written, so the page can be reloaded. */
async function settle(page: Page): Promise<void> {
  await setPowerSwitch(page, false);
  await page.evaluate(() => (window as any).__test.whenHddSaved());
}

/** Records every text the load overlay shows from here on, across reloads. */
async function recordOverlay(page: Page): Promise<void> {
  await page.addInitScript(() => {
    (window as any).__overlayTexts = [];
    document.addEventListener("DOMContentLoaded", () => {
      const label = document.getElementById("loadOverlayLabel");
      if (!label) return;
      new MutationObserver(() => (window as any).__overlayTexts.push(label.textContent)).observe(label, {
        childList: true, characterData: true, subtree: true,
      });
    });
  });
}

async function expectMarker(page: Page, text = /CONVERTED FROM 504MB/): Promise<void> {
  await waitForScreen(page, /C:\\>/);
  await focusScreen(page);
  await typeStr(page, "TYPE C:\\LEGACY\\MARKER.TXT");
  await waitForScreen(page, text);
}

// Stores legacy-504.img in an older shape: 512KB chunks under "hdd-meta" + "c-drive:<n>", or a
// factory-delta patched onto a "factory-base:<n>" stash (the delta rewrites the marker file to
// PATCHED). The raw-bytes shape isn't seeded: Chromium refuses IndexedDB values over ~133MB.
async function seedOlderLegacySave(page: Page, shape: "chunks" | "delta"): Promise<void> {
  await page.evaluate(
    async ({ dbName, store, shape }) => {
      const img = new Uint8Array(await (await fetch("tests/media/legacy-504.img")).arrayBuffer());
      const db: IDBDatabase = await new Promise((resolve, reject) => {
        const req = indexedDB.open(dbName, 1);
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
      });
      const chunkSize = 512 * 1024;
      const chunks = Math.ceil(img.length / chunkSize);
      await new Promise<void>((resolve, reject) => {
        const tx = db.transaction(store, "readwrite");
        const s = tx.objectStore(store);
        s.clear();
        const prefix = shape === "chunks" ? "c-drive:" : "factory-base:";
        for (let i = 0; i < chunks; i++) s.put(img.slice(i * chunkSize, (i + 1) * chunkSize), prefix + i);
        const meta = { v: 1, chunks, length: img.length, chunkSize };
        if (shape === "chunks") {
          s.put(meta, "hdd-meta");
        } else {
          s.put(meta, "factory-base-meta");
          const needle = new TextEncoder().encode("CONVERTED FROM 504MB");
          let at = -1;
          for (let i = img.indexOf(needle[0]); i >= 0; i = img.indexOf(needle[0], i + 1)) {
            if (needle.every((b, k) => img[i + k] === b)) { at = i; break; }
          }
          s.put({ v: 1, base: "factory", patches: [
            { offset: at, bytes: new TextEncoder().encode("PATCHED   FROM 504MB") },
          ] }, "c-drive");
        }
        tx.oncomplete = () => resolve();
        tx.onerror = () => reject(tx.error);
      });
      db.close();
    },
    { dbName: HDD_DB_NAME, store: HDD_STORE, shape },
  );
}

async function idbKeys(page: Page): Promise<string[]> {
  return page.evaluate(
    async ({ dbName, store }) => {
      const db: IDBDatabase = await new Promise((resolve, reject) => {
        const req = indexedDB.open(dbName, 1);
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
      });
      const keys = await new Promise<IDBValidKey[]>((resolve, reject) => {
        const req = db.transaction(store, "readonly").objectStore(store).getAllKeys();
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
      });
      db.close();
      return keys.map(String);
    },
    { dbName: HDD_DB_NAME, store: HDD_STORE },
  );
}

const chunkKeys = async (page: Page) => (await idbKeys(page)).filter((k) => k.startsWith("c2:"));

// Gives the page `__hddCall(op, args, transfer)`: one hdd-worker.js op in its own worker.
async function installHddCall(page: Page): Promise<void> {
  await page.evaluate(() => {
    (window as any).__hddCall = (op: string, args: unknown, transfer: Transferable[] = []) => {
      const w = new Worker("hdd-worker.js");
      return new Promise((resolve, reject) => {
        w.onmessage = (e) => {
          if (e.data.progress) return;
          w.terminate();
          if (e.data.ok) resolve(e.data.result);
          else reject(new Error(e.data.error));
        };
        w.onerror = (e) => reject(new Error(e.message));
        w.postMessage({ id: 1, op, args }, transfer);
      });
    };
  });
}

test.describe("hard disk", () => {
  test("shows the factory-default label and correct button states on first load", async ({
    page,
  }) => {
    // expectScreen: null asserts before boot finishes. FreeDOS writes C: during FDAUTO.BAT ~2s before
    // the prompt, so persistHddIfDirty()'s 5s tick relabels the drive "saved (this session)". PC486_REVIEW.md §8.
    await bootLive(page);
    await expect(page.locator("#hddStatus")).toHaveText(
      /Using: FreeDOS \(default\)/
    );
    await expect(page.locator("#hddDownloadBtn")).toBeEnabled();
    await expect(page.locator("#hddLegacyDownloadBtn")).toBeHidden();
    await expect(page.locator("#hddResetBtn")).toBeDisabled();
    await expect(page.locator("#hddBlankBtn")).toBeDisabled();
    await expect(page.locator("#hddUploadInput")).toBeDisabled();
  });

  test("the drive is a 256MB WD Caviar AC2250", async ({ livePage: page }) => {
    expect(await page.evaluate(() => (window as any).__test.machine.hddImage().length)).toBe(kHddImageBytes);
  });

  test("Reset to factory FreeDOS and Mount blank drive are only usable while powered off, and update the status label", async ({
    livePage: page,
  }) => {
    await setPowerSwitch(page, false);

    await expect(page.locator("#hddResetBtn")).toBeEnabled();
    await expect(page.locator("#hddBlankBtn")).toBeEnabled();

    await page.locator("#hddBlankBtn").click();
    await expect(page.locator("#hddStatus")).toHaveText(
      /blank drive \(unformatted\)/
    );
    await expect(page.locator("#hddStatus")).toHaveText(/Takes effect at next power-on/);

    await page.locator("#hddResetBtn").click();
    await expect(page.locator("#hddStatus")).toHaveText(
      /FreeDOS \(default\)/
    );
    await expect(page.locator("#hddStatus")).toHaveText(/Takes effect at next power-on/);
  });

  test("a blank drive takes effect next power-on and won't boot to a normal prompt", async ({
    livePage: page,
  }) => {
    await setPowerSwitch(page, false);
    await page.locator("#hddBlankBtn").click();
    await setPowerSwitch(page, true);

    await page.waitForFunction(() => !!(window as any).__test?.machine, null, {
      timeout: 15000,
    });
    await page.waitForTimeout(3000);

    expect(
      await page.evaluate(() => (window as any).__test.machine.textScreen())
    ).not.toMatch(/C:\\>/);

    // Restore factory FreeDOS so the shared livePage isn't left on a blank image.
    await setPowerSwitch(page, false);
    await page.locator("#hddResetBtn").click();
    await setPowerSwitch(page, true);
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, {
      timeout: 15000,
    });
  });

  test("Download image is enabled even while the machine is running", async ({
    livePage: page,
  }) => {

    await expect(page.locator("#hddDownloadBtn")).toBeEnabled();

    const [download] = await Promise.all([
      page.waitForEvent("download"),
      page.locator("#hddDownloadBtn").click(),
    ]);
    expect(download.suggestedFilename()).toBe("pc486-hdd.img");
  });

  test("C: is stored uncompressed in 64KB chunks", async ({ page }) => {
    await bootLive(page);
    await forcePersistHdd(page);
    const meta = await idbGet(page, META_KEY);
    expect(meta).toMatchObject({ v: 2, length: kHddImageBytes, chunkSize: 65536, model: "AC2250", modified: true });
    // Chunk 0 holds the MBR, so it is always stored.
    expect(await idbGet(page, "c2:00000")).toEqual({ arrayBuffer: 65536 });
    expect(await idbGet(page, LEGACY_KEY)).toBeUndefined();
  });

  test("C: persists across a page reload via IndexedDB", async ({
    page,
  }) => {
    await boot(page);
    await forcePersistHdd(page);
    await settle(page);
    await page.reload();

    await page.waitForFunction(() => !!(window as any).__test?.machine, null, {
      timeout: 15000,
    });
    await expect(page.locator("#hddStatus")).toHaveText(/saved \(previous visit\)/);
    await waitForScreen(page, /C:\\>/);
  });

  // Once C: lives in IndexedDB, a reload mounts it from there with no second image download.
  test("a reload with saved C: does not re-fetch the factory FreeDOS image", async ({
    page,
  }) => {
    await bootLive(page);
    await settle(page);

    // A HEAD for the image fingerprint (loadSavedHdd) is expected; only a GET means a re-download.
    const requests: { url: string; method: string }[] = [];
    page.on("request", (req) => requests.push({ url: req.url(), method: req.method() }));
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, {
      timeout: 15000,
    });
    expect(
      requests.some((r) => r.url.includes("freedos-hdd.img") && r.method === "GET")
    ).toBe(false);
  });

  // A visitor-changed C: is theirs; an untouched one follows the current factory image.
  test("a stale factory fingerprint replaces an untouched C: but keeps a changed one", async ({
    page,
  }) => {
    await bootLive(page);
    await settle(page);
    await idbPutMeta(page, { modified: false, factoryFp: "stale-fingerprint-from-a-previous-build" });

    let gets = 0;
    page.on("request", (r) => {
      if (r.url().includes("freedos-hdd.img") && r.method() === "GET") gets++;
    });
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 15000 });
    expect(gets).toBe(1);
    await expect(page.locator("#hddStatus")).toHaveText(/FreeDOS \(default\)/);

    await settle(page);
    await idbPutMeta(page, { modified: true, factoryFp: "stale-fingerprint-from-a-previous-build" });
    gets = 0;
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 15000 });
    expect(gets).toBe(0);
    await expect(page.locator("#hddStatus")).toHaveText(/saved \(previous visit\)/);
  });

  // Reset to factory means the image the server has now.
  test("Reset to factory re-fetches the image from the server, ignoring every local copy", async ({
    livePage: page,
  }) => {

    await setPowerSwitch(page, false);
    await expect(page.locator("#hddResetBtn")).toBeEnabled();

    const gets: string[] = [];
    page.on("request", (r) => {
      if (r.url().includes("freedos-hdd.img") && r.method() === "GET") gets.push(r.url());
    });

    await page.locator("#hddResetBtn").click();
    await expect.poll(() => gets.length, { timeout: 60000 }).toBeGreaterThan(0);
  });

  test("an old 504MB save converts to 256MB on load and keeps its files", async ({ page }) => {
    test.setTimeout(240_000);
    await bootLive(page);
    await settle(page);
    await seedLegacySave(page, "legacy-504");
    await recordOverlay(page);
    await page.reload();

    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 60000 });
    const texts: string[] = await page.evaluate(() => (window as any).__overlayTexts);
    expect(texts.some((t) => /Converting hard disk to 256MB/.test(t))).toBe(true);
    await expect(page.locator("#hddStatus")).toHaveText(/converted from 504MB/);
    expect(await page.evaluate(() => (window as any).__test.machine.hddImage().length)).toBe(kHddImageBytes);
    await expectMarker(page);

    expect(await idbGet(page, LEGACY_KEY)).toBeUndefined();
    expect(await idbGet(page, META_KEY)).toMatchObject({ v: 2, length: kHddImageBytes, modified: true });
  });

  test("an old save too big for 256MB is left alone and can still be downloaded", async ({ page }) => {
    test.setTimeout(240_000);
    await bootLive(page);
    await settle(page);
    await seedLegacySave(page, "legacy-504-full");
    await page.reload();

    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 60000 });
    await expect(page.locator("#hddStatus")).toHaveText(/couldn't be converted/);
    await expect(page.locator("#hddLegacyDownloadBtn")).toBeVisible();
    expect(await idbGet(page, LEGACY_KEY)).toEqual({ blob: expect.any(Number) });
    await waitForScreen(page, /C:\\>/);

    const download = await expectDownload(page, () => page.locator("#hddLegacyDownloadBtn").click());
    expect(download.suggestedFilename()).toBe("pc486-hdd-504mb.img");

    await setPowerSwitch(page, false);
    await page.locator("#hddResetBtn").click();
    await expect(page.locator("#hddLegacyDownloadBtn")).toBeHidden();
    await page.evaluate(() => (window as any).__test.whenHddSaved());
    expect(await idbGet(page, LEGACY_KEY)).toBeUndefined();
  });

  test("uploading a 504MB image converts it for the next power-on", async ({ page }) => {
    test.setTimeout(240_000);
    await bootLive(page);
    await setPowerSwitch(page, false);
    await page.locator("#hddUploadInput").setInputFiles("tests/media/legacy-504.img");
    await expect(page.locator("#hddStatus")).toHaveText(/converted from 504MB \(legacy-504\.img\)/, { timeout: 60000 });
    await setPowerSwitch(page, true);
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 30000 });
    await expectMarker(page);
  });

  test("uploading an image of the wrong size is refused", async ({ page }) => {
    await bootLive(page);
    await setPowerSwitch(page, false);
    const dialog = page.waitForEvent("dialog");
    await page.locator("#hddUploadInput").setInputFiles({
      name: "small.img", mimeType: "application/octet-stream", buffer: Buffer.alloc(4096),
    });
    const d = await dialog;
    expect(d.message()).toMatch(/255974400 bytes/);
    await d.dismiss();
    await expect(page.locator("#hddStatus")).not.toHaveText(/small\.img/);
  });
});

test.describe("hard disk storage and conversion", () => {
  for (const shape of ["chunks", "delta"] as const) {
    test(`an old 504MB save stored as ${shape} converts too`, async ({ page }) => {
      test.setTimeout(240_000);
      await bootLive(page);
      await settle(page);
      await seedOlderLegacySave(page, shape);
      await page.reload();

      await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 60000 });
      await expect(page.locator("#hddStatus")).toHaveText(/converted from 504MB/);
      await expectMarker(page, shape === "delta" ? /PATCHED\s+FROM 504MB/ : /CONVERTED FROM 504MB/);
      const keys = await idbKeys(page);
      expect(keys.filter((k) => !k.startsWith("c2:"))).toEqual([META_KEY]);
    });
  }

  test("the converter refuses disks it can't read, with a reason", async ({ page }) => {
    await page.goto("/?test=1");
    await page.addScriptTag({ url: "hdd-convert.js" });
    const results = await page.evaluate(() => {
      const convert = (window as any).convertLegacyHdd;
      const tryIt = (img: Uint8Array) => {
        try {
          convert(img);
          return "converted";
        } catch (err: any) {
          return err.code + ": " + err.message;
        }
      };
      const img = new Uint8Array(528482304);
      const dv = new DataView(img.buffer);
      const entry = (i: number, type: number, lba = 63, sectors = 1032129) => {
        img[446 + i * 16 + 4] = type;
        dv.setUint32(446 + i * 16 + 8, lba, true);
        dv.setUint32(446 + i * 16 + 12, sectors, true);
      };
      const out: Record<string, string> = {};
      out.wrongSize = tryIt(new Uint8Array(1000));
      out.noTable = tryIt(img);
      dv.setUint16(510, 0xAA55, true);
      out.noPartition = tryIt(img);
      entry(0, 0x0B);
      out.fat32 = tryIt(img);
      entry(0, 0x06);
      entry(1, 0x06, 600000, 1000);
      out.twoPartitions = tryIt(img);
      entry(1, 0x00, 0, 0);
      out.noBootSector = tryIt(img);
      dv.setUint16(63 * 512 + 510, 0xAA55, true);
      out.notFat16 = tryIt(img);
      return out;
    });
    expect(results).toEqual({
      wrongSize: "format: image is 1000 bytes, not 528482304",
      noTable: "format: no partition table",
      noPartition: "format: no partition",
      fat32: "format: partition type 0xb isn't FAT16",
      twoPartitions: "format: more than one partition",
      noBootSector: "format: no boot sector",
      notFat16: "format: not a FAT16 boot sector",
    });
  });

  test("Mount blank drive stores only a metadata record, no chunks", async ({ page }) => {
    await bootLive(page);
    await setPowerSwitch(page, false);
    await page.locator("#hddBlankBtn").click();
    await page.evaluate(() => (window as any).__test.whenHddSaved());
    expect(await idbGet(page, META_KEY)).toMatchObject({ v: 2, length: kHddImageBytes, modified: true });
    expect(await chunkKeys(page)).toEqual([]);
  });

  test("a save across a 64KB chunk boundary patches both chunks, and all-zero chunks are dropped", async ({ page }) => {
    await bootLive(page);
    await settle(page);
    await installHddCall(page);
    await page.evaluate((length) => (window as any).__hddCall("blank", { length }), kHddImageBytes);
    expect(await chunkKeys(page)).toEqual([]);

    const patched = await page.evaluate(() => (window as any).__hddCall("patch", {
      patches: [{ offset: 65534, bytes: new Uint8Array([1, 2, 3, 4]) }],
    }));
    expect(patched).toEqual({ needFull: false });
    expect(await chunkKeys(page)).toEqual(["c2:00000", "c2:00001"]);
    const around = await page.evaluate(async () => {
      const r = await (window as any).__hddCall("load", {});
      return Array.from(new Uint8Array(r.buffer, 65532, 8));
    });
    expect(around).toEqual([0, 0, 1, 2, 3, 4, 0, 0]);

    await page.evaluate(() => (window as any).__hddCall("patch", {
      patches: [{ offset: 65534, bytes: new Uint8Array(4) }],
    }));
    expect(await chunkKeys(page)).toEqual([]);
  });

  test("a save with nothing stored yet writes the whole image instead", async ({ page }) => {
    await bootLive(page);
    await page.evaluate(() => (window as any).__test.whenHddSaved());
    await installHddCall(page);
    await page.evaluate(() => (window as any).__hddCall("forget", {}));
    expect(await idbGet(page, META_KEY)).toBeUndefined();
    expect(await page.evaluate(() => (window as any).__hddCall("patch", { patches: [] }))).toEqual({ needFull: true });

    await forcePersistHdd(page);
    expect(await idbGet(page, META_KEY)).toMatchObject({ v: 2, length: kHddImageBytes, modified: true });
    expect(await idbGet(page, "c2:00000")).toEqual({ arrayBuffer: 65536 });
  });
});
