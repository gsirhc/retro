import { test, expect } from "@playwright/test";

// R8 ($4008 ALLPOT): cities 0-1, bonus credit 2, trackball 3, bonus city 4-6, cabinet 7.
// R10 ($4A00): coinage 0-1, right mult 2-3, center mult 4, language 5-6, unused 7. Factory is 0x73 / 0x81 (TM-147, rev 3).

test.describe("DIP switches", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await page.waitForFunction(() => (window as any).__test);
  });

  const dsw = (page: import("@playwright/test").Page) =>
    page.evaluate(() => {
      const m = (window as any).__test.machine;
      return { r8: m.r8(), r10: m.r10(), bus8: m.ramByte(0x4008), bus10: m.ramByte(0x4a00) };
    });

  test("factory default is 6 cities, 10,000 bonus city, mini trackball, 2 coins 1 play, English", async ({ page }) => {
    await expect(page.locator("#dipPanel")).toBeVisible();
    await expect(page.locator("#dipCities")).toHaveValue("3");
    await expect(page.locator("#dipBonusCredit")).toHaveValue("0");
    await expect(page.locator("#dipTrackball")).toHaveValue("0");
    await expect(page.locator("#dipBonusCity")).toHaveValue("112");
    await expect(page.locator("#dipCabinet")).toHaveValue("0");
    await expect(page.locator("#dipCoinage")).toHaveValue("1");
    await expect(page.locator("#dipRightMult")).toHaveValue("0");
    await expect(page.locator("#dipCenterMult")).toHaveValue("0");
    await expect(page.locator("#dipLanguage")).toHaveValue("0");
    await expect(page.locator("#dipSpare")).toHaveValue("128");
    await expect(page.locator("#dipSelfTest")).not.toBeChecked();
    expect(await dsw(page)).toEqual({ r8: 0x73, r10: 0x81, bus8: 0x73, bus10: 0x81 });
    await expect(page.locator("#dipPanel .legal")).toContainText(/physical switches/i);
  });

  const r8Cases: [string, string, string, number][] = [
    ["4 cities", "#dipCities", "2", 0x72],
    ["5 cities", "#dipCities", "1", 0x71],
    ["7 cities", "#dipCities", "0", 0x70],
    ["no bonus credit", "#dipBonusCredit", "4", 0x77],
    ["large trackball", "#dipTrackball", "8", 0x7b],
    ["bonus city 8,000", "#dipBonusCity", "16", 0x13],
    ["bonus city 12,000", "#dipBonusCity", "96", 0x63],
    ["bonus city 14,000", "#dipBonusCity", "80", 0x53],
    ["bonus city 15,000", "#dipBonusCity", "64", 0x43],
    ["bonus city 18,000", "#dipBonusCity", "48", 0x33],
    ["bonus city 20,000", "#dipBonusCity", "32", 0x23],
    ["no bonus city", "#dipBonusCity", "0", 0x03],
    ["cocktail", "#dipCabinet", "128", 0xf3],
  ];
  for (const [name, sel, value, r8] of r8Cases) {
    test(`${name} writes R8 = 0x${r8.toString(16)}`, async ({ page }) => {
      await page.locator(sel).selectOption(value);
      const d = await dsw(page);
      expect(d.r8).toBe(r8);
      expect(d.bus8).toBe(r8);
      expect(d.r10).toBe(0x81);
    });
  }

  const r10Cases: [string, string, string, number][] = [
    ["1 coin 1 play", "#dipCoinage", "0", 0x80],
    ["free play", "#dipCoinage", "2", 0x82],
    ["1 coin 2 plays", "#dipCoinage", "3", 0x83],
    ["right coin x4", "#dipRightMult", "4", 0x85],
    ["right coin x5", "#dipRightMult", "8", 0x89],
    ["right coin x6", "#dipRightMult", "12", 0x8d],
    ["center coin x2", "#dipCenterMult", "16", 0x91],
    ["French", "#dipLanguage", "32", 0xa1],
    ["German", "#dipLanguage", "64", 0xc1],
    ["Spanish", "#dipLanguage", "96", 0xe1],
    ["unused switch on", "#dipSpare", "0", 0x01],
  ];
  for (const [name, sel, value, r10] of r10Cases) {
    test(`${name} writes R10 = 0x${r10.toString(16)}`, async ({ page }) => {
      await page.locator(sel).selectOption(value);
      const d = await dsw(page);
      expect(d.r10).toBe(r10);
      expect(d.bus10).toBe(r10);
      expect(d.r8).toBe(0x73);
    });
  }

  test("a DIP change survives a reload", async ({ page }) => {
    await page.locator("#dipCities").selectOption("0");
    await page.locator("#dipCabinet").selectOption("128");
    await page.locator("#dipCoinage").selectOption("2");
    await page.locator("#dipLanguage").selectOption("96");
    expect(await dsw(page)).toMatchObject({ r8: 0xf0, r10: 0xe2 });

    await page.reload();
    await page.waitForFunction(() => (window as any).__test);
    await expect(page.locator("#dipCities")).toHaveValue("0");
    await expect(page.locator("#dipCabinet")).toHaveValue("128");
    await expect(page.locator("#dipCoinage")).toHaveValue("2");
    await expect(page.locator("#dipLanguage")).toHaveValue("96");
    expect(await dsw(page)).toMatchObject({ r8: 0xf0, r10: 0xe2 });
  });

  test("self-test switch pulls IN1 bit 0x40 low and survives a reload", async ({ page }) => {
    const in1 = () => page.evaluate(() => (window as any).__test.machine.ramByte(0x4900) & 0x40);
    expect(await in1()).toBe(0x40);
    await page.locator("#dipSelfTest").check();
    expect(await in1()).toBe(0x00);

    await page.reload();
    await page.waitForFunction(() => (window as any).__test);
    await expect(page.locator("#dipSelfTest")).toBeChecked();
    expect(await in1()).toBe(0x00);
  });
});
