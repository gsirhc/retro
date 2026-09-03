import { test, expect } from "./fixtures";
import { boot, waitForScreen, regs, memAt, panelRun, send, pickCatalogItem } from "./helpers";

test.describe("front-panel bootstrap guide", () => {
  test("toggles open and closed", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await expect(page.locator("#panelGuide .pg-body")).toBeHidden();
    await page.click("#pgToggle");
    await expect(page.locator("#panelGuide .pg-body")).toBeVisible();
    await page.click("#pgToggle");
    await expect(page.locator("#panelGuide .pg-body")).toBeHidden();
  });

  test("CP/M preset: the disk section shows the 0FF00h switch graphic and a Boot button", async ({
    page,
  }) => {
    await boot(page, { params: "preset=cpm" });
    await page.click("#pgToggle");
    const body = page.locator("#panelGuide .pg-body");
    await expect(body).toContainText(/boot the floppy/i);
    await expect(body).toContainText(/0FF00h/);
    await expect(body.locator(".pg-swrow")).toHaveCount(1); // the address switches
    // the switch graphic encodes 0xFF00: 8 'on' toggles (A15..A8), rest off
    const on = await body.locator(".pg-swrow .pg-sw.on").count();
    expect(on).toBe(8);
    await expect(page.locator("#pgBoot")).toBeVisible();
  });

  test("the Boot button in the guide brings up CP/M", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await page.click("#pgToggle");
    await page.click("#pgBoot");
    await waitForScreen(page, /A>/, 30_000);
  });

  test("tape preset: the guide lists the hand-key loader with an octal column", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await page.click("#pgToggle");
    const body = page.locator("#panelGuide .pg-body");
    await expect(body).toContainText(/load from paper tape/i);
    await expect(body.locator(".pg-listing table tr")).not.toHaveCount(0);
    await expect(body.locator(".pg-listing th", { hasText: "OCT" })).toBeVisible();
    await expect(page.locator("#pgKeyin")).toBeVisible();
    await expect(page.locator("#pgFeed")).toBeVisible();
  });

  test("'Key the loader in for me' writes the bootstrap at ramTop-0x40 and sets PC", async ({
    page,
  }) => {
    await boot(page, { params: "preset=stock" }); // 4 KB -> org = 0x1000 - 0x40 = 0x0FC0
    await page.click("#pgToggle");
    await page.click("#pgKeyin");
    const org = 4 * 1024 - 0x40;
    expect((await regs(page)).pc).toBe(org);
    expect(await memAt(page, org)).toBe(0x21); // LXI H,<dest>
    expect(await memAt(page, org + 3)).toBe(0x11); // LXI D,<count>
  });

  test("the guide's octal follows the machine's RAM size", async ({ page }) => {
    await boot(page, { params: "preset=stock" }); // 4 KB
    await page.click("#pgToggle");
    await expect(page.locator("#panelGuide .pg-body")).toContainText(/007 700|0FC0h|1\s*700/);

    await page.goto("/?test=1&preset=cpm");
    await page.waitForFunction(() => (window as any).__test?.applyingPreset === false);
    await page.click("#pgToggle");
    // 64 KB -> loader org 0xFFC0
    await expect(page.locator("#panelGuide .pg-body")).toContainText(/0FF00h/);
  });

  test("'Feed tape & run' spools a hand-keyed loader's tape", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await page.click("#pgToggle");
    await page.click("#pgKeyin");
    await page.click("#pgFeed");
    // the reader goes into its reading state and the CPU is running the loader
    await expect(page.locator("#ptr")).toHaveClass(/reading/, { timeout: 8000 });
    expect(await page.evaluate(() => (window as any).__test.running)).toBe(true);
  });

  test("the loader listing has no scrollbar (all rows inline)", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await page.click("#pgToggle");
    const oy = await page.evaluate(
      () => getComputedStyle(document.querySelector("#panelGuide .pg-listing")!).overflowY,
    );
    expect(oy).toBe("visible");
    // all 40 bootstrap rows are rendered (not virtualised / clipped)
    expect(await page.locator("#panelGuide .pg-listing table tr").count()).toBe(41); // header + 40
  });

  const swWord = (page) => page.evaluate(() => (window as any).__test.switchWord);

  test("per-row ↕ sets the data switches, then greys the row and locks it", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await page.click("#pgToggle");
    const row = (i: number) => page.locator(`#panelGuide .pg-listing table tr`).nth(i);
    await row(1).locator(".pg-do button").first().click(); // byte 0 = 0x21 (LXI H)
    expect(await swWord(page) & 0xff).toBe(0x21);
    expect(await swWord(page) & 0xff00).toBe(0); // sense switches untouched

    // that row is now a done checklist item
    await expect(row(1)).toHaveClass(/done/);
    await expect(row(1).locator('.pg-do button[data-act="set"]')).toBeDisabled();
    await expect(row(1).locator('.pg-do button[data-act="dep"]')).toBeDisabled();
    await expect(row(1).locator(".pg-checkbtn")).toHaveClass(/on/);

    await row(4).locator(".pg-do button").first().click(); // a different row still works
    expect(await swWord(page) & 0xff).toBe(0x11); // byte 3 = 0x11 (LXI D)
  });

  test("the ✓ button toggles a row done / not-done by hand", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await page.click("#pgToggle");
    const row = page.locator("#panelGuide .pg-listing table tr").nth(5);
    await row.locator(".pg-checkbtn").click();
    await expect(row).toHaveClass(/done/);
    await expect(row.locator('.pg-do button[data-act="dep"]')).toBeDisabled();
    // untick -> row comes back
    await row.locator(".pg-checkbtn").click();
    await expect(row).not.toHaveClass(/done/);
    await expect(row.locator('.pg-do button[data-act="dep"]')).toBeEnabled();
  });

  test("'Key the loader in for me' ticks the whole checklist", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await page.click("#pgToggle");
    await page.click("#pgKeyin");
    const rows = page.locator("#panelGuide .pg-listing table tr");
    const n = (await rows.count()) - 1;
    for (let i = 1; i <= n; i++) await expect(rows.nth(i)).toHaveClass(/done/);
  });

  test("per-row ▸ sets the switches and deposits the byte, advancing PC", async ({ page }) => {
    await boot(page, { params: "preset=stock" }); // org = 0x0FC0
    const org = 4 * 1024 - 0x40;
    await page.click("#pgToggle");
    const dep = (i: number) =>
      page.locator(`#panelGuide .pg-listing table tr`).nth(i).locator('.pg-do button[data-act="dep"]');
    await dep(1).click(); // deposit byte 0
    expect(await swWord(page) & 0xff).toBe(0x21);
    expect(await memAt(page, org)).toBe(0x21);
    expect((await regs(page)).pc).toBe(org + 1);
    await dep(2).click(); // deposit byte 1 (lo(dest) = 0)
    expect(await memAt(page, org + 1)).toBe(0x00);
    expect((await regs(page)).pc).toBe(org + 2);
    await dep(8).click(); // deposit byte 7 = 0x10
    expect(await memAt(page, org + 7)).toBe(0x10);
  });

  test("the address-row ↕ sets the address switches", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    const org = 4 * 1024 - 0x40;
    await page.click("#pgToggle");
    const addr = page.locator("#panelGuide .pg-body ol .pg-do").first();
    await addr.locator('button[data-act="set"]').click();
    expect(await swWord(page)).toBe(org);
    await expect(addr).toHaveClass(/done/);
  });

  test("the address-row ▸ sets the switches and flips EXAMINE (PC = org)", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    const org = 4 * 1024 - 0x40;
    await page.click("#pgToggle");
    const addr = page.locator("#panelGuide .pg-body ol .pg-do").first();
    await addr.locator('button[data-act="exam"]').click();
    expect(await swWord(page)).toBe(org);
    expect((await regs(page)).pc).toBe(org);
  });

  test("the disk section's address buttons target 0FF00h", async ({ page }) => {
    await boot(page, { params: "preset=cpm" });
    await page.click("#pgToggle");
    const addr = page.locator("#panelGuide .pg-body ol .pg-do").first();
    await addr.locator('button[data-act="exam"]').click(); // set switches + EXAMINE
    expect(await swWord(page)).toBe(0xff00);
    expect((await regs(page)).pc).toBe(0xff00);
    await panelRun(page);
    await waitForScreen(page, /A>/, 30_000);
  });

  test("the guide is a floating panel that starts on the right and can be dragged", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await page.click("#pgToggle");
    const panel = page.locator("#panelGuide .pg-body");
    expect(await panel.evaluate((el) => getComputedStyle(el).position)).toBe("fixed");
    const vw = page.viewportSize()!.width;
    let box = (await panel.boundingBox())!;
    expect(box.x + box.width).toBeGreaterThan(vw - 40); // hugs the right edge
    expect(box.x).toBeGreaterThan(vw / 2); // not covering the panel on the left

    // drag the title bar to the left
    const bar = page.locator("#pgDrag");
    const bb = (await bar.boundingBox())!;
    await page.mouse.move(bb.x + 40, bb.y + bb.height / 2);
    await page.mouse.down();
    await page.mouse.move(120, 220, { steps: 8 });
    await page.mouse.up();
    box = (await panel.boundingBox())!;
    expect(box.x).toBeLessThan(200);
    expect(await page.evaluate(() => JSON.parse(localStorage.getItem("retro8080.pgpos") || "{}").left))
      .toBeLessThan(200);

    // the dragged position survives a guide rebuild (preset change)
    await page.selectOption("#preset", "");
    await expect(panel).toBeVisible();
    expect((await panel.boundingBox())!.x).toBeLessThan(200);

    // ...and a full reload (restored from localStorage)
    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine);
    await page.click("#pgToggle");
    expect((await page.locator("#panelGuide .pg-body").boundingBox())!.x).toBeLessThan(200);
  });

  test("the guide's × button closes it", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await page.click("#pgToggle");
    await expect(page.locator("#panelGuide .pg-body")).toBeVisible();
    await page.click("#pgX");
    await expect(page.locator("#panelGuide .pg-body")).toBeHidden();
    await expect(page.locator("#pgToggle")).toContainText("Load it yourself");
  });

  test("hand-keying every byte with ▸ then Feed streams 4K BASIC in", async ({ page }) => {
    await boot(page, { params: "preset=stock" });
    await page.click("#pgToggle");
    const rows = page.locator("#panelGuide .pg-listing table tr");
    const n = (await rows.count()) - 1; // minus header
    for (let i = 1; i <= n; i++) {
      await rows.nth(i).locator('.pg-do button[data-act="dep"]').click();
    }
    expect(await memAt(page, 0xfc0)).toBe(0x21); // whole loader is in memory
    // the stock preset already threaded 4K BASIC in the reader -- just Feed it.
    // A hand-keyed loader does no prompt-answering, so BASIC stops at its first
    // cold-start question -- exactly as it would on real hardware.
    await page.click("#pgFeed");
    await waitForScreen(page, /MEMORY SIZE\?/i, 40_000);
    // finish the cold start by hand (the hand-keyed loader answers nothing)
    await send(page, "\r");
    await waitForScreen(page, /TERMINAL WIDTH\?/i);
    await send(page, "\r");
    await waitForScreen(page, /SIN\?/i);
    await send(page, "Y\r");
    await waitForScreen(page, /\bOK\b/, 20_000);
  });

  test("keying the loader in, then threading a different tape, re-keys it to match", async ({
    page,
  }) => {
    // Bare-Metal threads Kill the Bit (~24 bytes) -- the loader's baked-in byte
    // count would be wrong for anything else
    await boot(page, { params: "preset=baremetal" });
    await page.click("#pgToggle");
    await page.click("#pgKeyin");
    const org = 4 * 1024 - 0x40;
    const countAt = async () => (await memAt(page, org + 4)) + ((await memAt(page, org + 5)) << 8);
    expect(await countAt()).toBeLessThan(64); // baked for Kill the Bit

    await panelRun(page); // loader starts polling the 2SIO

    // swap in the 4K BASIC tape and Feed it -- the flow the user hit
    await page.click("#ptr .ptr-window");
    await pickCatalogItem(page, "#ptrList", /4K BASIC 4\.0/);
    await page.click("#ptrThread");
    expect(await countAt()).toBeGreaterThan(3000); // the loader followed the tape

    await page.click("#ptr .ptr-start");
    await waitForScreen(page, /MEMORY SIZE\?/i, 40_000); // 4K BASIC cold-starts
  });
});
