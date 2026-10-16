import { test, expect } from "./fixtures";
import { boot, emuWait, focusScreen, typeStr, waitForScreen } from "./helpers";

type Page = import("@playwright/test").Page;

const WARNING = "Warning: the browser blocked storage (IndexedDB). Changes to C: won't be there when you return.";
const WORKER = "shared/hdd-worker.js?model=WD1003&db=ibmpcat-hdd";

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

// True when the worker's stored C: matches the running machine byte for byte.
function storedMatchesMachine(page: Page): Promise<boolean> {
  return page.evaluate((url) => new Promise<boolean>((resolve, reject) => {
    const w = new Worker(url);
    w.onmessage = (e) => {
      w.terminate();
      if (!e.data.ok) return reject(new Error(e.data.error));
      if (!e.data.result.buffer) return resolve(false);
      const stored = new Uint8Array(e.data.result.buffer);
      const live = (window as any).__test.machine.hddImage();
      resolve(stored.length === live.length && stored.every((b, i) => b === live[i]));
    };
    w.postMessage({ id: 1, op: "load", args: {} });
  }), WORKER);
}

async function writeToC(page: Page): Promise<void> {
  await focusScreen(page);
  await typeStr(page, "md saved");
  await waitForScreen(page, /MD SAVED\s+C:\\>\s*$/i);
  // FreeDOS flushes its buffers a moment after the prompt returns.
  await emuWait(page, 3000);
}

test.describe("C: storage", () => {
  test("with IndexedDB blocked the factory C: still boots and the label warns", async ({ page }) => {
    await blockableIdb(page, true);
    await boot(page);
    await expect(page.locator("#hddStatus")).toHaveText("Using: factory FreeDOS (default). " + WARNING);
  });

  test("a save storage refuses says so, and is retried in full once storage works", async ({ page }) => {
    await blockableIdb(page, false);
    await boot(page);
    await setBlocked(page, true);
    await page.evaluate(() => {
      const el = document.getElementById("hddStatus")!;
      const seen: string[] = (window as any).__hddLabels = [];
      new MutationObserver(() => seen.push(el.textContent || "")).observe(el, { childList: true, characterData: true, subtree: true });
    });
    await writeToC(page);
    await page.evaluate(() => (window as any).__test.persistHdd());
    const shown: string[] = await page.evaluate(() => (window as any).__hddLabels);
    expect(shown.length).toBeGreaterThan(0);
    expect(shown.filter((t) => t.includes("saved state"))).toEqual([]);
    await expect(page.locator("#hddStatus")).toHaveText("Using: changes from this session. " + WARNING);

    await setBlocked(page, false);
    // Checked and retried in one task, so the guest can't dirty C: in between.
    const retried = await page.evaluate(async () => {
      const t = (window as any).__test;
      const dirty = t.machine.hddDirty();
      await t.retryHddSave();
      return { dirty, label: document.getElementById("hddStatus")!.textContent };
    });
    expect(retried).toEqual({ dirty: false, label: "Using: saved state (changes from this session)" });
    expect(await storedMatchesMachine(page)).toBe(true);
  });

  test("after the first full save, later saves send only the written sectors", async ({ page }) => {
    await page.addInitScript(() => {
      const ops: string[] = (window as any).__hddOps = [];
      const post = Worker.prototype.postMessage;
      Worker.prototype.postMessage = function (msg: any, ...rest: any[]) {
        if (msg && msg.op) ops.push(msg.op);
        return post.call(this, msg, ...(rest as [any]));
      };
    });
    await boot(page);
    await writeToC(page);
    await page.evaluate(() => (window as any).__test.persistHdd());
    await expect(page.locator("#hddStatus")).toHaveText("Using: saved state (changes from this session)");
    expect(await storedMatchesMachine(page)).toBe(true);

    await page.evaluate(() => { (window as any).__hddOps.length = 0; });
    await typeStr(page, "md again");
    await waitForScreen(page, /MD AGAIN\s+C:\\>\s*$/i);
    await emuWait(page, 3000);
    await page.evaluate(() => (window as any).__test.persistHdd());
    const ops: string[] = await page.evaluate(() => (window as any).__hddOps);
    expect(ops).toContain("patch");
    expect(ops).not.toContain("replace");
    expect(await storedMatchesMachine(page)).toBe(true);
  });
});
