import { test, expect } from "./fixtures";
import { boot, focusScreen, setPowerSwitch } from "./helpers";

type Page = import("@playwright/test").Page;

const noticeVisible = (page: Page) =>
  page.locator("#bootNotice").evaluate((el) => el.classList.contains("visible"));

async function powerCycle(page: Page): Promise<void> {
  await setPowerSwitch(page, false);
  await setPowerSwitch(page, true);
  await page.waitForFunction(() => !!(window as any).__test?.machine);
}

test.describe("\"Barebones FreeDOS\" boot notice", () => {
  test("shows over the factory C:, dismisses on focus, and stays dismissed", async ({ page }) => {
    await boot(page, { expectScreen: null });
    expect(await noticeVisible(page)).toBe(true);
    await expect(page.locator("#bootNotice")).toContainText("Barebones FreeDOS");

    await focusScreen(page);
    expect(await noticeVisible(page)).toBe(false);
    expect(await page.evaluate(() => localStorage.getItem("retro8080.ibmpcatBootNoticeDismissed"))).toBe("1");

    await powerCycle(page);
    expect(await noticeVisible(page)).toBe(false);

    await page.evaluate(() => localStorage.clear());
    await powerCycle(page);
    expect(await noticeVisible(page)).toBe(true);
  });

  test("power-off hides it without marking it seen", async ({ page }) => {
    await boot(page, { expectScreen: null });
    expect(await noticeVisible(page)).toBe(true);
    await setPowerSwitch(page, false);
    expect(await noticeVisible(page)).toBe(false);
    expect(await page.evaluate(() => localStorage.getItem("retro8080.ibmpcatBootNoticeDismissed"))).toBeNull();
  });

  test("does not show over the Keen preset C:", async ({ page }) => {
    await boot(page, { params: "disk=keen1", expectScreen: null });
    expect(await noticeVisible(page)).toBe(false);
  });

  test("does not show over a C: other than the factory one", async ({ page }) => {
    await boot(page, { expectScreen: null });
    await setPowerSwitch(page, false);
    await page.locator("#hddBlankBtn").click();
    await setPowerSwitch(page, true);
    await page.waitForFunction(() => !!(window as any).__test?.machine);
    expect(await noticeVisible(page)).toBe(false);
  });
});
