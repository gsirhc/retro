import { test, expect, type Page } from "@playwright/test";
import { readFileSync } from "fs";
import { ROM_DB, HWTEST_FILES, USER_BYTE } from "../cabinet";

const BLOCKED = "Warning: Your ROM has been loaded but the browser blocked storage (IndexedDB).  It won't reload when you return.";

// The hwtest set with one program byte flipped: a complete set arcade-core files as the visitor's own.
function userSet() {
  return HWTEST_FILES.map((path, i) => {
    const buffer = Buffer.from(readFileSync(path));
    if (i === 0) buffer[USER_BYTE] ^= 1;
    return { name: path.split("/").pop()!, mimeType: "application/octet-stream", buffer };
  });
}

test("an unreadable stored set falls back to the test ROM and says so", async ({ page }) => {
  await page.goto("/?test=1");
  await page.waitForFunction(() => (window as any).__test);
  await page.evaluate((db) => new Promise((resolve, reject) => {
    const req = indexedDB.open(db, 1);
    req.onupgradeneeded = () => req.result.createObjectStore("roms");
    req.onsuccess = () => {
      const tx = req.result.transaction("roms", "readwrite");
      tx.objectStore("roms").put({ kind: "user" }, (window as any).__test.ROM_KEY);
      tx.oncomplete = () => { req.result.close(); resolve(null); };
      tx.onerror = () => reject(tx.error);
    };
    req.onerror = () => reject(req.error);
  }), ROM_DB);
  await page.reload();
  await page.waitForFunction(() => (window as any).__test);
  await expect(page.locator("#romStatus")).toHaveText("Stored ROM unreadable. Using test ROM");
  await expect(page.locator("#romErrorHint")).toBeVisible();
  expect(await page.evaluate(() => (window as any).__test.usingUserRom)).toBe(false);
});

function storedKind(page: Page) {
  return page.evaluate((db) => new Promise((resolve) => {
    const req = indexedDB.open(db, 1);
    req.onupgradeneeded = () => req.result.createObjectStore("roms");
    req.onsuccess = () => {
      const g = req.result.transaction("roms").objectStore("roms").get("set");
      g.onsuccess = () => { req.result.close(); resolve(g.result ? g.result.kind : ""); };
      g.onerror = () => { req.result.close(); resolve(""); };
    };
    req.onerror = () => resolve("");
  }), ROM_DB);
}

test("the page's open store gives way to deleteDatabase and reopens after", async ({ page }) => {
  await page.goto("/?test=1");
  await page.waitForFunction(() => (window as any).__test);
  await page.locator("#romFile").setInputFiles(HWTEST_FILES);
  await expect.poll(() => storedKind(page)).toBe("hwtest");
  const result = await page.evaluate((db) => new Promise((resolve) => {
    const req = indexedDB.deleteDatabase(db);
    req.onsuccess = () => resolve("deleted");
    req.onblocked = () => resolve("blocked");
  }), ROM_DB);
  expect(result).toBe("deleted");

  await page.locator("#removeRomBtn").click();
  await page.locator("#romFile").setInputFiles(HWTEST_FILES);
  await expect.poll(() => storedKind(page)).toBe("hwtest");
});

test.describe("with IndexedDB blocked", () => {
  test.beforeEach(async ({ page }) => {
    await page.addInitScript(() => {
      IDBFactory.prototype.open = () => { throw new DOMException("blocked", "SecurityError"); };
    });
    await page.goto("/?test=1");
    await page.waitForFunction(() => (window as any).__test);
  });

  test("the test ROM still boots and runs", async ({ page }) => {
    const c0 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    await expect.poll(() => page.evaluate(() => (window as any).__test.machine.totalCycles())).toBeGreaterThan(c0);
    expect(await page.evaluate(() => (window as any).__test.usingUserRom)).toBe(false);
  });

  test("a user set loads for the session with the same warning", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText(BLOCKED);
    expect(await page.evaluate(() => (window as any).__test.usingUserRom)).toBe(true);
    const c0 = await page.evaluate(() => (window as any).__test.machine.totalCycles());
    await expect.poll(() => page.evaluate(() => (window as any).__test.machine.totalCycles())).toBeGreaterThan(c0);
  });

  test("a loaded set runs for the session and Remove ROMs still reverts", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect.poll(() => page.evaluate(() => (window as any).__test.usingUserRom)).toBe(true);
    await expect(page.locator("#romStatus")).toHaveText(BLOCKED);
    await page.locator("#removeRomBtn").click();
    await expect.poll(() => page.evaluate(() => (window as any).__test.usingUserRom)).toBe(false);
    await expect(page.locator("#romStatus")).toHaveText("Running test ROM");
    await page.waitForFunction(() => !!(window as any).__test?.machine);
  });
});
