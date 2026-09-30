import { test, expect } from "./fixtures";
import { screenText } from "./helpers";

// Doom 1.2 and its contemporaries predate WASD -- they default to the arrow
// cluster. The mapping happens at the browser edge, so the guest receives
// genuine arrow-key scancodes; it is off by default, like every other
// convenience control on this page.
test.describe("WASD to arrow keys", () => {
  test("is off by default and never restored from a saved preference", async ({ page }) => {
    await page.goto("/?test=1");
    const box = page.locator("#wasdArrows");
    await expect(box).not.toBeChecked();
    await box.check();
    await page.reload();
    await expect(page.locator("#wasdArrows")).not.toBeChecked();
  });

  test("off: W types a literal w at the DOS prompt", async ({ promptPage: page }) => {
    await page.locator("#screen").click();
    await page.keyboard.press("w");
    await expect.poll(() => screenText(page), { timeout: 10_000 }).toMatch(/C:\\>w/);
  });

  test("on: W no longer types a w -- it becomes the up arrow", async ({ promptPage: page }) => {
    await page.locator("#screen").click();
    // FreeCom treats ArrowUp as command-history recall, not a no-op -- so
    // seed a distinctive entry, then map W→Up and expect that recall (not
    // the letter "w" typed onto a bare prompt).
    await page.keyboard.type("xyzzy");
    await page.keyboard.press("Enter");
    await expect.poll(() => screenText(page), { timeout: 10_000 }).toMatch(/xyzzy/i);
    await page.locator("#wasdArrows").check();
    await page.locator("#screen").click();
    await page.keyboard.press("w");
    await expect
      .poll(async () => {
        const last = ((await screenText(page)).trimEnd().split(/\n/).pop() || "").trimEnd();
        return last;
      }, { timeout: 10_000 })
      .toMatch(/^C:\\>xyzzy$/i);
  });

  test("keys held across a toggle are released, not left stuck down", async ({ promptPage: page }) => {
    await page.locator("#screen").click();
    await page.keyboard.down("w");            // tracked as ArrowUp once mapped
    await page.locator("#wasdArrows").check();
    await page.keyboard.up("w");
    // Whatever was held must have been broken; typing still works normally.
    await page.locator("#screen").click();
    await page.keyboard.press("x");
    await expect.poll(() => screenText(page), { timeout: 10_000 }).toMatch(/x/);
  });

  // A and D strafe rather than turn: Doom's strafe modifier is Alt
  // (key_strafe), so they send Alt with the arrow.
  test("A and D map to Alt+arrow so they strafe, W and S stay plain arrows", async ({
    promptPage: page,
  }) => {
    const map = async (code: string) =>
      page.evaluate((c) => (window as any).__test.mapKey(c), code);

    expect(await map("KeyA")).toEqual(["KeyA"]);   // off: untouched

    await page.locator("#wasdArrows").check();
    expect(await map("KeyW")).toEqual(["ArrowUp"]);
    expect(await map("KeyS")).toEqual(["ArrowDown"]);
    expect(await map("KeyA")).toEqual(["AltLeft", "ArrowLeft"]);
    expect(await map("KeyD")).toEqual(["AltLeft", "ArrowRight"]);
  });

  test("on: A no longer types a literal a at the DOS prompt", async ({ promptPage: page }) => {
    await page.locator("#wasdArrows").check();
    await page.locator("#screen").click();
    await page.keyboard.press("a");
    await page.waitForTimeout(500);
    expect((await screenText(page)).trimEnd().split(/\n/).pop() || "").not.toMatch(/a\s*$/i);
  });
});
