import { test, expect } from "./fixtures";
import { bootLive } from "./helpers";

// The Sound Blaster 16's backplate volume wheel (PC486_REVIEW.md section 30):
// an analog pot after the card's output amp, so DOS can't see it. app.js
// gives it a square-law taper up to kWheelMaxGain (4.0) and remembers the
// position in retro8080.pc486SbVolume.
const kKey = "retro8080.pc486SbVolume";

async function wheelGain(page: any) {
  return page.evaluate(() => (window as any).__test.sbWheelGain);
}

test.describe("Sound Blaster 16 volume wheel", () => {
  test("starts at 70 and sets the output gain on a square-law taper", async ({ page }) => {
    await bootLive(page);
    await expect(page.locator("#sbVolume")).toHaveValue("70");
    await expect(page.locator("#sbVolumeReadout")).toHaveText("70");

    await page.locator("#speakerEnabled").check();
    await expect.poll(() => wheelGain(page)).not.toBeNull();
    expect(await wheelGain(page)).toBeCloseTo(0.7 * 0.7 * 4.0, 5);

    await page.locator("#sbVolume").fill("100");
    await expect(page.locator("#sbVolumeReadout")).toHaveText("100");
    expect(await wheelGain(page)).toBeCloseTo(4.0, 5);

    await page.locator("#sbVolume").fill("0");
    await expect(page.locator("#sbVolumeReadout")).toHaveText("0");
    expect(await wheelGain(page)).toBe(0);
    await page.evaluate((k) => localStorage.removeItem(k), kKey);
  });

  test("its position persists across reload", async ({ page }) => {
    await bootLive(page);
    await page.locator("#sbVolume").fill("35");
    expect(await page.evaluate((k) => localStorage.getItem(k), kKey)).toBe("35");

    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, {
      timeout: 15000,
    });
    await expect(page.locator("#sbVolume")).toHaveValue("35");
    await expect(page.locator("#sbVolumeReadout")).toHaveText("35");
    await page.evaluate((k) => localStorage.removeItem(k), kKey);
  });

  test("an out-of-range saved position is ignored", async ({ page }) => {
    await page.addInitScript((k) => localStorage.setItem(k, "250"), kKey);
    await bootLive(page);
    await expect(page.locator("#sbVolume")).toHaveValue("70");
    await expect(page.locator("#sbVolumeReadout")).toHaveText("70");
  });
});
