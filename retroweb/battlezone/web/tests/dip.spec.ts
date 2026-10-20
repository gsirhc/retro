import { test, expect } from "@playwright/test";

// DSW0 and DSW1 factory values are 0x15 and 0x63 (operator manual fig. 7 and 8).

test.describe("DIP switches", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await page.waitForFunction(() => !!(window as any).__test?.machine);
  });

  const dsw0 = (page: import("@playwright/test").Page) => page.evaluate(() => (window as any).__test.machine.dsw0());
  const dsw1 = (page: import("@playwright/test").Page) => page.evaluate(() => (window as any).__test.machine.dsw1());

  test("factory default is 3 tanks, missile at 10000, 15k/100k bonus, English, 2 coins / 1 play, 6 credits for 4 coins", async ({ page }) => {
    await expect(page.locator("#dipPanel")).toBeVisible();
    await expect(page.locator("#dipLives")).toHaveValue("1");
    await expect(page.locator("#dipMissile")).toHaveValue("4");
    await expect(page.locator("#dipBonus")).toHaveValue("16");
    await expect(page.locator("#dipLanguage")).toHaveValue("0");
    await expect(page.locator("#dipCoinage")).toHaveValue("3");
    await expect(page.locator("#dipRightMech")).toHaveValue("0");
    await expect(page.locator("#dipLeftMech")).toHaveValue("0");
    await expect(page.locator("#dipBonusCoins")).toHaveValue("96");
    await expect(page.locator("#dipSelfTest")).not.toBeChecked();
    expect(await dsw0(page)).toBe(0x15);
    expect(await dsw1(page)).toBe(0x63);
    await expect(page.locator("#dipPanel .legal")).toContainText(/physical switches/i);
  });

  const dsw0Cases: [string, string, string, number][] = [
    ["2 tanks", "#dipLives", "0", 0x14],
    ["4 tanks", "#dipLives", "2", 0x16],
    ["5 tanks", "#dipLives", "3", 0x17],
    ["missile at 5000", "#dipMissile", "0", 0x11],
    ["missile at 20000", "#dipMissile", "8", 0x19],
    ["missile at 30000", "#dipMissile", "12", 0x1d],
    ["bonus tank 25000", "#dipBonus", "32", 0x25],
    ["bonus tank 50000", "#dipBonus", "48", 0x35],
    ["no bonus tank", "#dipBonus", "0", 0x05],
    ["German", "#dipLanguage", "64", 0x55],
    ["French", "#dipLanguage", "128", 0x95],
    ["Spanish", "#dipLanguage", "192", 0xd5],
  ];
  for (const [name, sel, value, want] of dsw0Cases) {
    test(`${name} writes DSW0 = 0x${want.toString(16)}`, async ({ page }) => {
      await page.locator(sel).selectOption({ value });
      expect(await dsw0(page)).toBe(want);
      expect(await dsw1(page)).toBe(0x63);
    });
  }

  const dsw1Cases: [string, string, string, number][] = [
    ["free play", "#dipCoinage", "0", 0x60],
    ["1 coin / 2 credits", "#dipCoinage", "1", 0x61],
    ["1 coin / 1 credit", "#dipCoinage", "2", 0x62],
    ["right mech x4", "#dipRightMech", "4", 0x67],
    ["right mech x5", "#dipRightMech", "8", 0x6b],
    ["right mech x6", "#dipRightMech", "12", 0x6f],
    ["left mech x2", "#dipLeftMech", "16", 0x73],
    ["no bonus coins", "#dipBonusCoins", "0", 0x03],
    ["bonus coins 3 for 2", "#dipBonusCoins", "32", 0x23],
    ["bonus coins 5 for 4", "#dipBonusCoins", "64", 0x43],
    ["bonus coins 6 for 5", "#dipBonusCoins", "128", 0x83],
  ];
  for (const [name, sel, value, want] of dsw1Cases) {
    test(`${name} writes DSW1 = 0x${want.toString(16)}`, async ({ page }) => {
      await page.locator(sel).selectOption({ value });
      expect(await dsw1(page)).toBe(want);
      expect(await dsw0(page)).toBe(0x15);
    });
  }

  test("the encoders match what the machine holds", async ({ page }) => {
    await page.locator("#dipLives").selectOption({ value: "3" });
    await page.locator("#dipBonusCoins").selectOption({ value: "64" });
    expect(await page.evaluate(() => (window as any).__test.encodeDsw0())).toBe(await dsw0(page));
    expect(await page.evaluate(() => (window as any).__test.encodeDsw1())).toBe(await dsw1(page));
  });

  test("changes are saved to localStorage and survive a reload", async ({ page }) => {
    await page.locator("#dipLives").selectOption({ value: "3" });
    await page.locator("#dipCoinage").selectOption({ value: "0" });
    expect(await dsw0(page)).toBe(0x17);
    expect(await dsw1(page)).toBe(0x60);
    expect(JSON.parse((await page.evaluate(() => localStorage.getItem("retroweb.battlezone.dips")))!))
      .toEqual({ dsw0: 0x17, dsw1: 0x60, selfTest: false });

    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine);
    await expect(page.locator("#dipLives")).toHaveValue("3");
    await expect(page.locator("#dipCoinage")).toHaveValue("0");
    expect(await dsw0(page)).toBe(0x17);
    expect(await dsw1(page)).toBe(0x60);
  });

  test("self-test switch clears IN0 bit 0x10 and survives a reload", async ({ page }) => {
    const in0 = () => page.evaluate(() => (window as any).__test.machine.in0());
    expect(await in0()).toBe(0x3f);
    await page.locator("#dipSelfTest").check();
    expect(await in0()).toBe(0x2f);

    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine);
    await expect(page.locator("#dipSelfTest")).toBeChecked();
    expect(await in0()).toBe(0x2f);
  });

  test("Diag step clears IN0 bit 0x20 while held", async ({ page }) => {
    const in0 = () => page.evaluate(() => (window as any).__test.machine.in0());
    const btn = page.locator("#diagStep");
    await btn.dispatchEvent("pointerdown", { button: 0, pointerId: 1 });
    await expect.poll(in0).toBe(0x1f);
    await btn.dispatchEvent("pointerup", { button: 0, pointerId: 1 });
    await expect.poll(in0).toBe(0x3f);
  });
});
