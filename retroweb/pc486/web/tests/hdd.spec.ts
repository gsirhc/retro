import { test, expect } from "./fixtures";
import { boot, bootLive, waitForScreen, setPowerSwitch } from "./helpers";

// Hard disk (C: fixed drive) controls: reset/blank/download/upload operations
// take effect only on next power-on since a real WD1003 can't be swapped live.
// Buttons disabled while running, upload/reset/blank also disabled until firmware
// loads. C: state persists across reloads via IndexedDB.

/** Snapshot C: into IndexedDB even if FreeDOS never dirtied it this boot.
 * Factory-derived images persist as a small delta record (not a 504MB put). */
async function forcePersistHdd(page: import("@playwright/test").Page): Promise<void> {
  await page.evaluate(async () => {
    await (window as any).__test.forcePersistHdd();
    await (window as any).__test.whenHddSaved();
  });
  await expect(page.locator("#hddStatus")).toHaveText(/saved \(this session\)/);
}

// Mirrors app.js's own openHddDb()/HDD_STORE -- there's no other seam to
// reach the factory-image fingerprint record from a test, and the fix
// itself is explicitly meant to survive exactly this kind of direct
// IndexedDB inspection/mutation (see ensureFactoryHdd's comment).
const HDD_DB_NAME = "pc486-hdd";
const HDD_STORE = "hdd";
const FACTORY_META_KEY = "factory-base-meta";

async function idbGet(page: import("@playwright/test").Page, key: string): Promise<any> {
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
      return val;
    },
    { dbName: HDD_DB_NAME, store: HDD_STORE, key },
  );
}

async function idbPut(page: import("@playwright/test").Page, key: string, value: unknown): Promise<void> {
  await page.evaluate(
    async ({ dbName, store, key, value }) => {
      const db: IDBDatabase = await new Promise((resolve, reject) => {
        const req = indexedDB.open(dbName, 1);
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
      });
      await new Promise<void>((resolve, reject) => {
        const tx = db.transaction(store, "readwrite");
        tx.objectStore(store).put(value, key);
        tx.oncomplete = () => resolve();
        tx.onerror = () => reject(tx.error);
      });
      db.close();
    },
    { dbName: HDD_DB_NAME, store: HDD_STORE, key, value },
  );
}

/** Rewrite the recorded factory-image fingerprint to something the live
 * server will never match, simulating a visitor whose stash predates a
 * freedos-hdd.img rebuild. Waits for app.js's own fire-and-forget
 * recordFactoryFingerprint() write to land first so it can't race back in
 * over the corruption. */
async function corruptFactoryFingerprint(page: import("@playwright/test").Page): Promise<void> {
  await expect
    .poll(async () => (await idbGet(page, FACTORY_META_KEY))?.fp, { timeout: 15_000 })
    .toEqual(expect.any(String));
  const meta = await idbGet(page, FACTORY_META_KEY);
  await idbPut(page, FACTORY_META_KEY, { ...meta, fp: "stale-fingerprint-from-a-previous-build" });
}

const HDD_KEY = "c-drive";

/** Write a small stand-in "full image" C: record directly, instead of
 * driving hddBlankBtn/hddUploadInput -- those save the real 504MB image as
 * one monolithic IndexedDB put, which app.js's own comment on
 * isFactoryDeltaRecord() notes "froze the main thread for minutes in
 * Playwright" (that's exactly why a normal dirty tick never does this).
 * A short buffer mounts safely: wd1003.cpp bounds-checks every transfer
 * against the actual image length and reports a clean IDNF past the end
 * (CLAUDE.md's "fail safely" contract) rather than reading out of bounds,
 * and this test only needs the record's *shape* (a Uint8Array, not a
 * factory-delta object) for the boot flow to treat it as a full-image save. */
async function putFakeFullImageSave(page: import("@playwright/test").Page): Promise<void> {
  await page.evaluate(
    async ({ dbName, store, key }) => {
      const db: IDBDatabase = await new Promise((resolve, reject) => {
        const req = indexedDB.open(dbName, 1);
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
      });
      await new Promise<void>((resolve, reject) => {
        const tx = db.transaction(store, "readwrite");
        tx.objectStore(store).put(new Uint8Array(4096), key);
        tx.oncomplete = () => resolve();
        tx.onerror = () => reject(tx.error);
      });
      db.close();
    },
    { dbName: HDD_DB_NAME, store: HDD_STORE, key: HDD_KEY },
  );
}

test.describe("hard disk", () => {
  test("shows the factory-default label and correct button states on first load", async ({
    page,
  }) => {
    // expectScreen: null -- assert on first load, before the machine has
    // finished booting, which is what this test is actually about. Waiting
    // for the C:\> prompt would defeat it: FreeDOS genuinely writes to C:
    // while running FDAUTO.BAT, ~2s of real time BEFORE the prompt appears,
    // so by then persistHddIfDirty()'s 5s tick has correctly relabelled the
    // drive "saved (this session)". See PC486_REVIEW.md §8.
    await bootLive(page);
    await expect(page.locator("#hddStatus")).toHaveText(
      /Using: FreeDOS \(default\)/
    );
    await expect(page.locator("#hddDownloadBtn")).toBeEnabled();
    await expect(page.locator("#hddResetBtn")).toBeDisabled();
    await expect(page.locator("#hddBlankBtn")).toBeDisabled();
    await expect(page.locator("#hddUploadInput")).toBeDisabled();
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

    // Put factory FreeDOS back so the shared livePage isn't left on a blank
    // image (status becomes "saved (this session)" after mount, which resetLivePage
    // cannot distinguish from a normal dirty factory disk).
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

  test("C: persists across a page reload via IndexedDB", async ({
    page,
  }) => {
    await boot(page);
    await forcePersistHdd(page);
    await setPowerSwitch(page, false);
    await page.evaluate(() => (window as any).__test.whenHddSaved());
    await page.reload();

    await page.waitForFunction(() => !!(window as any).__test?.machine, null, {
      timeout: 15000,
    });
    await expect(page.locator("#hddStatus")).toHaveText(/saved \(previous visit\)/);
    await waitForScreen(page, /C:\\>/);
  });

  // Mirrors the CD-ROM "never fetches unasked" case: once C: lives in
  // IndexedDB as a factory-delta, a reload reconstructs from the stashed
  // factory image + patches -- no second download of freedos-hdd.img.
  test("a reload with saved C: does not re-fetch the factory FreeDOS image", async ({
    page,
  }) => {
    // Factory stash is a one-time 504MB IndexedDB put of the already-fetched
    // ArrayBuffer (no wasm copy); give it room on a loaded host.
    test.setTimeout(300_000);
    await boot(page);
    await forcePersistHdd(page);
    await page.evaluate(async () => {
      await (window as any).__test.whenFactoryStashed();
    });
    await setPowerSwitch(page, false);
    await page.evaluate(() => (window as any).__test.whenHddSaved());

    // A HEAD to confirm the shipped image's identity hasn't changed (see
    // ensureFactoryHdd's fingerprint check) is expected and cheap -- only a
    // GET would mean the 504MB image was actually re-downloaded.
    const requests: { url: string; method: string }[] = [];
    page.on("request", (req) => requests.push({ url: req.url(), method: req.method() }));
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, {
      timeout: 15000,
    });
    await expect(page.locator("#hddStatus")).toHaveText(/saved \(previous visit\)/);
    expect(
      requests.some((r) => r.url.includes("freedos-hdd.img") && r.method === "GET")
    ).toBe(false);
  });

  // A full-image save (blank drive, upload, or any non-factory-delta
  // record) is self-contained -- it doesn't reference the factory image at
  // all, so a changed fingerprint must not touch it.
  test("a saved full-image C: survives a stale factory-image fingerprint untouched", async ({
    page,
  }) => {
    await boot(page);
    await putFakeFullImageSave(page);
    await corruptFactoryFingerprint(page);

    const requests: { url: string; method: string }[] = [];
    page.on("request", (req) => requests.push({ url: req.url(), method: req.method() }));
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, {
      timeout: 15000,
    });

    // A full-image record never consults the factory image on mount -- no
    // HEAD, no GET, and definitely not a reset to factory defaults.
    expect(requests.some((r) => r.url.includes("freedos-hdd.img"))).toBe(false);
    await expect(page.locator("#hddStatus")).toHaveText(/saved \(previous visit\)/);
  });

  // "Reset to factory" has to mean the image the server has right now. The
  // stash and the Cache API copy are optimisations for ordinary page loads;
  // trusting either here would hand back whatever was downloaded first, which
  // is exactly how a changed factory image failed to reach a visitor.
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
});
