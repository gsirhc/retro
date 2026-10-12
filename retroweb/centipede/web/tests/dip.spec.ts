import { test, expect } from "@playwright/test";

// DSW1 ($0800): language 0-1, lives 2-3, bonus 4-5, difficulty 6. DSW2 ($0801): coinage 0-1. Factory is 0x54 / 0x02 (MAME centiped).

test.describe("DIP switches", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
    await page.waitForFunction(() => (window as any).__test);
  });

  const dsw = (page: import("@playwright/test").Page) =>
    page.evaluate(() => {
      const m = (window as any).__test.machine;
      return { dsw1: m.dsw1(), dsw2: m.dsw2(), bus1: m.ramByte(0x0800), bus2: m.ramByte(0x0801) };
    });

  test("factory default is 3 lives, 12,000 bonus, easy, English, 1C/1C", async ({ page }) => {
    await expect(page.locator("#dipPanel")).toBeVisible();
    await expect(page.locator("#dipLives")).toHaveValue("4");
    await expect(page.locator("#dipBonus")).toHaveValue("16");
    await expect(page.locator("#dipDifficulty")).toHaveValue("64");
    await expect(page.locator("#dipLanguage")).toHaveValue("0");
    await expect(page.locator("#dipCoinage")).toHaveValue("2");
    await expect(page.locator("#dipSelfTest")).not.toBeChecked();
    expect(await dsw(page)).toEqual({ dsw1: 0x54, dsw2: 0x02, bus1: 0x54, bus2: 0x02 });
    await expect(page.locator("#dipPanel .legal")).toContainText(/physical switches/i);
  });

  const dsw1Cases: [string, string, string, number][] = [
    ["2 lives", "#dipLives", "0", 0x50],
    ["4 lives", "#dipLives", "8", 0x58],
    ["5 lives", "#dipLives", "12", 0x5c],
    ["bonus 10,000", "#dipBonus", "0", 0x44],
    ["bonus 15,000", "#dipBonus", "32", 0x64],
    ["bonus 20,000", "#dipBonus", "48", 0x74],
    ["hard", "#dipDifficulty", "0", 0x14],
    ["German", "#dipLanguage", "1", 0x55],
    ["French", "#dipLanguage", "2", 0x56],
    ["Spanish", "#dipLanguage", "3", 0x57],
  ];
  for (const [name, sel, value, dsw1] of dsw1Cases) {
    test(`${name} writes DSW1 = 0x${dsw1.toString(16)}`, async ({ page }) => {
      await page.locator(sel).selectOption(value);
      const d = await dsw(page);
      expect(d.dsw1).toBe(dsw1);
      expect(d.bus1).toBe(dsw1);
      expect(d.dsw2).toBe(0x02);
    });
  }

  const dsw2Cases: [string, string, number][] = [
    ["free play", "0", 0x00],
    ["1C/2C", "1", 0x01],
    ["2C/1C", "3", 0x03],
  ];
  for (const [name, value, dsw2] of dsw2Cases) {
    test(`coinage ${name} writes DSW2 = 0x${dsw2.toString(16).padStart(2, "0")}`, async ({ page }) => {
      await page.locator("#dipCoinage").selectOption(value);
      const d = await dsw(page);
      expect(d.dsw2).toBe(dsw2);
      expect(d.bus2).toBe(dsw2);
      expect(d.dsw1).toBe(0x54);
    });
  }

  test("a DIP change survives a reload", async ({ page }) => {
    await page.locator("#dipLives").selectOption("12");
    await page.locator("#dipDifficulty").selectOption("0");
    await page.locator("#dipCoinage").selectOption("0");
    expect(await dsw(page)).toMatchObject({ dsw1: 0x1c, dsw2: 0x00 });

    await page.reload();
    await page.waitForFunction(() => (window as any).__test);
    await expect(page.locator("#dipLives")).toHaveValue("12");
    await expect(page.locator("#dipDifficulty")).toHaveValue("0");
    await expect(page.locator("#dipCoinage")).toHaveValue("0");
    expect(await dsw(page)).toMatchObject({ dsw1: 0x1c, dsw2: 0x00 });
  });

  test("self-test switch pulls IN0 bit 0x20 low and survives a reload", async ({ page }) => {
    const service = () => page.evaluate(() => (window as any).__test.machine.ramByte(0x0c00) & 0x30);
    expect(await service()).toBe(0x20);
    await page.locator("#dipSelfTest").check();
    expect(await service()).toBe(0x00);

    await page.reload();
    await page.waitForFunction(() => (window as any).__test);
    await expect(page.locator("#dipSelfTest")).toBeChecked();
    expect(await service()).toBe(0x00);
  });
});
