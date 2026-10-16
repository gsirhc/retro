import { test, expect } from "./fixtures";
import { boot, focusScreen, typeStr, waitForScreen } from "./helpers";

type Page = import("@playwright/test").Page;

const WARNING = "Warning: the browser blocked storage (IndexedDB). Changes to C: won't be there when you return.";

// window.__idbBlocked makes every IndexedDB open throw, like a browser that refuses storage.
async function blockableIdb(page: Page, blocked: boolean): Promise<void> {
  await page.addInitScript((b) => {
    (window as any).__idbBlocked = b;
    const open = IDBFactory.prototype.open;
    IDBFactory.prototype.open = function (...args: any[]) {
      if ((window as any).__idbBlocked) throw new DOMException("blocked", "SecurityError");
      return open.apply(this, args as [string]);
    };
  }, blocked);
}

async function writeToC(page: Page): Promise<void> {
  await focusScreen(page);
  await typeStr(page, "md saved", { pressEnterAfter: true });
  await expect.poll(() => page.evaluate(() => (window as any).__test.machine.hddDirty())).toBe(true);
}

async function storedImage(page: Page): Promise<number> {
  return page.evaluate(() => new Promise<number>((resolve) => {
    const req = indexedDB.open("ibmpcat-hdd", 1);
    req.onsuccess = () => {
      const get = req.result.transaction("hdd").objectStore("hdd").get("c-drive");
      get.onsuccess = () => resolve(get.result ? get.result.byteLength : 0);
    };
  }));
}

test.describe("C: storage", () => {
  test("with IndexedDB blocked the factory C: still boots and the label warns", async ({ page }) => {
    await blockableIdb(page, true);
    await boot(page);
    await expect(page.locator("#hddStatus")).toHaveText("Using: factory FreeDOS (default). " + WARNING);
  });

  test("a save storage refuses says so, and is retried once storage works", async ({ page }) => {
    await blockableIdb(page, false);
    await boot(page);
    await page.evaluate(() => { (window as any).__idbBlocked = true; });
    await writeToC(page);
    const shown = await page.evaluate(async () => {
      const el = document.getElementById("hddStatus")!;
      const seen: string[] = [];
      const obs = new MutationObserver(() => seen.push(el.textContent || ""));
      obs.observe(el, { childList: true, characterData: true, subtree: true });
      await (window as any).__test.persistHdd();
      obs.disconnect();
      return seen;
    });
    expect(shown.filter((t) => t.includes("saved state"))).toEqual([]);
    await expect(page.locator("#hddStatus")).toHaveText("Using: changes from this session. " + WARNING);
    expect(await page.evaluate(() => (window as any).__test.machine.hddDirty())).toBe(false);

    await page.evaluate(() => { (window as any).__idbBlocked = false; });
    await page.evaluate(() => (window as any).__test.retryHddSave());
    await expect(page.locator("#hddStatus")).toHaveText("Using: saved state (changes from this session)");
    expect(await storedImage(page)).toBe(31900160);
  });

  test("a save that commits labels C: saved and stores the image", async ({ page }) => {
    await blockableIdb(page, false);
    await boot(page);
    await writeToC(page);
    await page.evaluate(() => (window as any).__test.persistHdd());
    await expect(page.locator("#hddStatus")).toHaveText("Using: saved state (changes from this session)");
    expect(await storedImage(page)).toBe(31900160);
    await waitForScreen(page, /C:\\>\s*$/m);
  });
});
