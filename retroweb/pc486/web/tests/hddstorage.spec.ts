import { test, expect } from "./fixtures";
import { boot, bootLive, focusScreen, typeStr, waitForScreen, emuWait } from "./helpers";

type Page = import("@playwright/test").Page;

const kHddImageBytes = 255974400;
const WARNING = "Warning: the browser blocked storage (IndexedDB). Changes to C: won't be there when you return.";

// self.__idbBlocked makes IndexedDB in the hard disk worker throw, like a browser that refuses storage.
async function blockableIdb(page: Page, blocked: boolean): Promise<void> {
  await page.route("**/hdd-worker.js*", async (route) => {
    const res = await route.fetch();
    const prelude = `self.__idbBlocked = ${blocked};
const __open = IDBFactory.prototype.open;
IDBFactory.prototype.open = function (...a) {
  if (self.__idbBlocked) throw new DOMException("blocked", "SecurityError");
  return __open.apply(this, a);
};
const __tx = IDBDatabase.prototype.transaction;
IDBDatabase.prototype.transaction = function (...a) {
  if (self.__idbBlocked) throw new DOMException("blocked", "SecurityError");
  return __tx.apply(this, a);
};
`;
    await route.fulfill({ response: res, body: prelude + (await res.text()) });
  });
}

async function setBlocked(page: Page, blocked: boolean): Promise<void> {
  await expect.poll(() => page.workers().length).toBeGreaterThan(0);
  for (const w of page.workers()) {
    if (w.url().includes("hdd-worker.js")) await w.evaluate((b) => { (self as any).__idbBlocked = b; }, blocked);
  }
}

// True when the worker's stored C: matches the running machine's byte for byte.
function storedMatchesMachine(page: Page): Promise<boolean> {
  return page.evaluate(() => new Promise<boolean>((resolve, reject) => {
    const w = new Worker("shared/hdd-worker.js?convert=1&model=AC2250&db=pc486-hdd");
    w.onmessage = (e) => {
      if (e.data.progress) return;
      w.terminate();
      if (!e.data.ok) return reject(new Error(e.data.error));
      const stored = new Uint8Array(e.data.result.buffer);
      const live = (window as any).__test.machine.hddImage();
      resolve(stored.length === live.length && stored.every((b, i) => b === live[i]));
    };
    w.postMessage({ id: 1, op: "load", args: {} });
  }));
}

test.describe("C: storage", () => {
  test("with IndexedDB blocked the factory C: still boots and the label warns", async ({ page }) => {
    await blockableIdb(page, true);
    await bootLive(page);
    await expect(page.locator("#hddStatus")).toHaveText("Using: FreeDOS (default). " + WARNING);
    expect(await page.evaluate(() => (window as any).__test.machine.hddImage().length)).toBe(kHddImageBytes);
  });

  test("a save storage refuses says so, and is retried in full once storage works", async ({ page }) => {
    await blockableIdb(page, false);
    await boot(page);
    await page.evaluate(() => (window as any).__test.forcePersistHdd());
    await setBlocked(page, true);
    await page.evaluate(() => {
      const el = document.getElementById("hddStatus")!;
      const seen: string[] = (window as any).__hddLabels = [];
      new MutationObserver(() => seen.push(el.textContent || "")).observe(el, { childList: true, characterData: true, subtree: true });
    });
    await focusScreen(page);
    await typeStr(page, "MD SAVED");
    await waitForScreen(page, /MD SAVED\s+C:\\>\s*$/i);
    // FreeDOS flushes its buffers a moment after the prompt returns.
    await emuWait(page, 3000);
    await page.evaluate(() => (window as any).__test.persistHdd());
    const shown: string[] = await page.evaluate(() => (window as any).__hddLabels);
    expect(shown.length).toBeGreaterThan(0);
    expect(shown.filter((t) => t.includes("saved"))).toEqual([]);
    await expect(page.locator("#hddStatus")).toHaveText("Using: changes from this session. " + WARNING);

    await setBlocked(page, false);
    // Checked and retried in one task, so the guest can't dirty C: in between.
    const retried = await page.evaluate(async () => {
      const t = (window as any).__test;
      const dirty = t.machine.hddDirty();
      await t.retryHddSave();
      return { dirty, label: document.getElementById("hddStatus")!.textContent };
    });
    expect(retried).toEqual({ dirty: false, label: "Using: saved (this session)" });
    expect(await storedMatchesMachine(page)).toBe(true);
  });
});
