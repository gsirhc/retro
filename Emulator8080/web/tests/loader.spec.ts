import { test, expect } from "./fixtures";
import {
  boot,
  waitForScreen,
  screen,
  send,
  regs,
  pickCatalogItem,
  hasCatalogItem,
} from "./helpers";
import * as path from "path";

async function threadAndLoad(page, label: RegExp) {
  await page.click("#ptr .ptr-window");
  await pickCatalogItem(page, "#ptrList", label);
  await page.click("#ptrThread");
  await expect(page.locator("#ptr")).toHaveClass(/loaded/);
  await page.click("#ptr .ptr-load");
}

test.describe("program loader (paper-tape reader dialog)", () => {
  test("the catalog offers the built-in demo ROMs", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await page.click("#ptr .ptr-window");
    expect(await hasCatalogItem(page, "#ptrList", /Serial echo/)).toBe(true);
    expect(await hasCatalogItem(page, "#ptrList", /Hello, world/)).toBe(true);
    expect(await hasCatalogItem(page, "#ptrList", /Kill the Bit/)).toBe(true);
  });

  test("picking an entry fills in its load and start addresses", async ({ page }) => {
    await boot(page, { params: "preset=cassette" });
    await page.click("#ptr .ptr-window");
    await pickCatalogItem(page, "#ptrList", /8K BASIC 4\.0/);
    await expect(page.locator("#ptrAddr")).toHaveValue(/0x0*0/i);
    await expect(page.locator("#ptrStart")).toHaveValue(/0x0*0/i);
  });

  test("'Hello, world' prints its greeting and halts", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await threadAndLoad(page, /Hello, world/);
    await expect.poll(() => screen(page), { timeout: 10_000 }).toMatch(/\w/);
    await expect.poll(() => regs(page).then((s) => s.halted), { timeout: 10_000 }).toBe(true);
  });

  test("'Serial echo' echoes typed characters", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await threadAndLoad(page, /Serial echo/);
    await page.waitForFunction(() => (window as any).__test.tape.phase === "idle", null, {
      timeout: 10_000,
    });
    await send(page, "XYZ99");
    await expect.poll(() => screen(page)).toMatch(/XYZ99/);
  });

  test("the multi-file 8K BASIC ROM build concatenates and cold-starts", async ({ page }) => {
    await boot(page, { params: "preset=cassette" }); // 32K
    await threadAndLoad(page, /8K BASIC \(ROM build\)/);
    await waitForScreen(page, /\bOK\b/, 40_000);
  });

  test("Choose File loads a local binary at the given address", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await page.click("#ptr .ptr-window");
    await page.setInputFiles("#ptrFile", path.join(__dirname, "..", "roms", "hello.bin"));
    await page.fill("#ptrAddr", "0x0000");
    await page.fill("#ptrStart", "0x0000");
    await page.click("#ptrThread");
    await expect(page.locator("#ptr")).toHaveClass(/loaded/);
    await page.click("#ptr .ptr-load");
    await expect.poll(() => regs(page).then((s) => s.halted), { timeout: 10_000 }).toBe(true);
  });
});
