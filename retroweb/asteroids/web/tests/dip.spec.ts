import { test, expect } from "@playwright/test";

// DSW1 packs four 2-bit fields at $2800..$2803. Factory value is 0x12.

test.describe("DIP switches", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await page.waitForFunction(() => (window as any).__test);
  });

  test("factory default is 1C/1C, x1/x1 mech, 3 lives, English (DSW1 = 0x12)", async ({ page }) => {
    await expect(page.locator("#dipPanel")).toBeVisible();
    await expect(page.locator("#dipCoinage")).toHaveValue("2");
    await expect(page.locator("#dipRightMech")).toHaveValue("0");
    await expect(page.locator("#dipCenterMech")).toHaveValue("0");
    await expect(page.locator("#dipLives")).toHaveValue("16");
    await expect(page.locator("#dipLanguage")).toHaveValue("0");
    await expect(page.locator("#dipSelfTest")).not.toBeChecked();
    expect(await page.evaluate(() => (window as any).__test.machine.dsw1())).toBe(0x12);
    await expect(page.locator("#dipPanel .legal")).toContainText(/physical switches/i);
  });

  const dsw1Cases: [string, string, string, number][] = [
    ["coinage free play", "#dipCoinage", "0", 0x10],
    ["coinage 1C/2C", "#dipCoinage", "1", 0x11],
    ["coinage 2C/1C", "#dipCoinage", "3", 0x13],
    ["right mech x4", "#dipRightMech", "4", 0x16],
    ["right mech x6", "#dipRightMech", "12", 0x1e],
    ["4 lives", "#dipLives", "0", 0x02],
    ["center mech x2", "#dipCenterMech", "32", 0x32],
    ["German", "#dipLanguage", "64", 0x52],
    ["French", "#dipLanguage", "128", 0x92],
    ["Spanish", "#dipLanguage", "192", 0xd2],
  ];
  for (const [name, sel, value, dsw1] of dsw1Cases) {
    test(`${name} writes DSW1 = 0x${dsw1.toString(16)}`, async ({ page }) => {
      await page.locator(sel).selectOption(value);
      expect(await page.evaluate(() => (window as any).__test.machine.dsw1())).toBe(dsw1);
    });
  }

  test("a DSW1 change survives a reload", async ({ page }) => {
    await page.locator("#dipLives").selectOption("0");
    await page.locator("#dipCoinage").selectOption("0");
    expect(await page.evaluate(() => (window as any).__test.machine.dsw1())).toBe(0x00);

    await page.reload();
    await page.waitForFunction(() => (window as any).__test);
    await expect(page.locator("#dipLives")).toHaveValue("0");
    await expect(page.locator("#dipCoinage")).toHaveValue("0");
    expect(await page.evaluate(() => (window as any).__test.machine.dsw1())).toBe(0x00);
  });

  test("self-test switch sets IN0 bit 0x80 and survives a reload", async ({ page }) => {
    expect(await page.evaluate(() => (window as any).__test.machine.in0())).toBe(0);
    await page.locator("#dipSelfTest").check();
    expect(await page.evaluate(() => (window as any).__test.machine.in0())).toBe(0x80);

    await page.reload();
    await page.waitForFunction(() => (window as any).__test);
    await expect(page.locator("#dipSelfTest")).toBeChecked();
    expect(await page.evaluate(() => (window as any).__test.machine.in0())).toBe(0x80);
  });
});
