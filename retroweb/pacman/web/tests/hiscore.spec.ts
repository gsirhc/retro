import { test, expect } from "@playwright/test";
import { existsSync, readFileSync } from "fs";
import path from "path";

// HIGH SCORE is always persisted for a user ROM. Reset HIGH SCORE is the
// labelled way back to a power-cycle empty table. See PACMAN_REVIEW.md §9.
// Fixtures are the generated self-test ROM, never Namco Pac-Man.

const HWTEST_FILES = [
  "roms/program.bin",
  "roms/pacman.5e",
  "roms/pacman.5f",
  "roms/82s123.7f",
  "roms/82s126.4a",
  "roms/82s126.1m",
];

// Shared origin IndexedDB (ROM set + hiscores) — keep the whole file
// serial so a real-ROM load cannot race the CRC-patched hwtest fixtures.
test.describe.configure({ mode: "serial" });

function userSet() {
  return HWTEST_FILES.map((path) => {
    const name = path.split("/").pop()!;
    let buffer = readFileSync(path);
    if (name === "program.bin") {
      buffer = Buffer.from(buffer);
      buffer[0] ^= 1;
    }
    return { name, mimeType: "application/octet-stream" as const, buffer };
  });
}

test.describe("HIGH SCORE persist", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/?test=1");
  });

  test("Reset is present and disabled on the self-test ROM", async ({ page }) => {
    const btn = page.locator("#resetHiscore");
    await expect(btn).toBeVisible();
    await expect(btn).toBeDisabled();
    await expect(page.locator("p.legal").filter({ hasText: "HIGH SCORE is kept" })).toBeVisible();
  });

  test("TOP RAM is saved and poked back after a reload", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    await expect(page.locator("#resetHiscore")).toBeEnabled();

    await page.evaluate(async () => {
      const t = (window as any).__test;
      t.writeHiscore([0x00, 0x76, 0x00]);
      await t.saveHiscoreNow(t.readHiscore());
    });

    await page.reload();
    await expect(page.locator("#romStatus")).toHaveText("Loaded stored ROM set");

    const bytes = await page.evaluate(async () => {
      await (window as any).__test.restoreHiscoreNow();
      return (window as any).__test.readHiscore();
    });
    expect(bytes).toEqual([0x00, 0x76, 0x00]);
  });

  test("Reset Cancel leaves the saved TOP in place", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    await page.evaluate(async () => {
      const t = (window as any).__test;
      t.writeHiscore([0x00, 0x50, 0x00]);
      await t.saveHiscoreNow(t.readHiscore());
    });

    await page.locator("#resetHiscore").click();
    await expect(page.locator("#hiscoreResetHint")).toBeVisible();
    await page.locator("#hiscoreResetCancel").click();
    await expect(page.locator("#hiscoreResetHint")).toBeHidden();

    const bytes = await page.evaluate(async () => {
      return (window as any).__test.readHiscore();
    });
    expect(bytes).toEqual([0x00, 0x50, 0x00]);
  });

  test("Reset confirm clears the saved TOP", async ({ page }) => {
    await page.locator("#romFile").setInputFiles(userSet());
    await expect(page.locator("#romStatus")).toHaveText("Loaded ROM set");
    await page.evaluate(async () => {
      const t = (window as any).__test;
      t.writeHiscore([0x00, 0x76, 0x00]);
      await t.saveHiscoreNow(t.readHiscore());
    });

    await page.locator("#resetHiscore").click();
    await page.locator("#hiscoreResetOk").click();
    await expect(page.locator("#hiscoreResetHint")).toBeHidden();

    const bytes = await page.evaluate(async () => {
      const t = (window as any).__test;
      await t.resetHiscoreNow();
      t.writeHiscore([0x99, 0x99, 0x99]);
      await t.restoreHiscoreNow();
      return t.readHiscore();
    });
    expect(bytes).toEqual([0x99, 0x99, 0x99]);
  });
});

function firstExisting(...candidates: (string | undefined)[]) {
  for (const p of candidates) {
    if (p && existsSync(p)) return p;
  }
}

// Real Namco dumps are opt-in and local-only (roms/user/ or PACMAN_ROM /
// MSPACMAN_ROM). CI has none, so this describe skips there.
function localUserRom(): { zip: string; url: string } | undefined {
  const pac = firstExisting(
    process.env.PACMAN_ROM,
    path.resolve(__dirname, "../../roms/user/pacman.zip"),
    path.resolve(__dirname, "../../roms/user/puckman.zip"),
  );
  if (pac) return { zip: pac, url: "/?test=1" };
  const ms = firstExisting(
    process.env.MSPACMAN_ROM,
    path.resolve(__dirname, "../../roms/user/mspacman.zip"),
  );
  if (ms) return { zip: ms, url: "/?game=mspacman&test=1" };
}

// Midway #2ACE: 6 nibbles from $4E8A down, leading zeros → tile $40, C starts at 4.
function expectedHiscoreTiles(bytes: number[]) {
  const tiles: number[] = [];
  let c = 4;
  for (let i = 2; i >= 0; i--) {
    const b = bytes[i] & 0xff;
    for (const nib of [(b >> 4) & 0xf, b & 0xf]) {
      if (nib !== 0) { c = 0; tiles.push(nib); }
      else if (c === 0) tiles.push(0);
      else { tiles.push(0x40); c--; }
    }
  }
  return tiles;
}

test.describe("HIGH SCORE attract display (real ROM)", () => {
  test("saved TOP is painted on attract after reload", async ({ page }) => {
    const rom = localUserRom();
    test.skip(!rom, "no local pacman/mspacman zip — drop one in roms/user/ or set PACMAN_ROM / MSPACMAN_ROM");
    test.setTimeout(90_000);

    const score = [0x00, 0x76, 0x00];
    const wantTiles = expectedHiscoreTiles(score);
    const mspac = rom!.url.includes("mspacman");

    await page.goto(rom!.url);
    await page.waitForFunction(() => (window as any).__test);
    await page.evaluate(() => (window as any).__test.wipeStorage());
    await page.reload();
    await page.waitForFunction(() => (window as any).__test);
    await expect(page.locator("#romStatus")).toHaveText("Running test ROM");

    await page.locator("#romFile").setInputFiles(rom!.zip);
    await expect(page.locator("#romStatus")).toHaveText(
      mspac ? "Midway mspacman ROM set" : /Midway pacman ROM set|Loaded ROM set/,
    );
    expect(await page.evaluate(() => (window as any).__test.usingUserRom)).toBe(true);
    if (mspac) {
      expect(await page.evaluate(() => (window as any).__test.machine.auxBoard())).toBe(true);
    }

    await page.waitForFunction(() => {
      const t = (window as any).__test;
      return t && t.usingUserRom && t.mode === 1 && t.machine.state().frames >= 480;
    }, null, { timeout: 30_000 });

    const immediate = await page.evaluate((bytes) => {
      const t = (window as any).__test;
      t.writeHiscore(bytes);
      return { ram: t.readHiscore(), tiles: t.hiscoreTiles() };
    }, score);
    expect(immediate.ram).toEqual(score);
    expect(immediate.tiles).toEqual(wantTiles);

    const saved = await page.evaluate(async (bytes) => {
      const t = (window as any).__test;
      await t.saveHiscoreNow(bytes);
      return { key: t.hiscoreKey, all: await t.loadSavedHiscores() };
    }, score);
    expect(saved.all[saved.key], `IDB after save: ${JSON.stringify(saved)}`).toEqual(score);

    await page.reload();
    await page.waitForFunction(() => (window as any).__test);
    await expect(page.locator("#romStatus")).toHaveText("Loaded stored ROM set");

    try {
      await page.waitForFunction((want) => {
        const t = (window as any).__test;
        if (!t || !t.usingUserRom || !t.hiscoreRestored) return false;
        const ram = t.readHiscore();
        const tiles = t.hiscoreTiles();
        return ram[0] === want.score[0] && ram[1] === want.score[1] && ram[2] === want.score[2]
          && tiles[2] === want.tiles[2] && tiles[3] === want.tiles[3]
          && tiles[4] === want.tiles[4] && tiles[5] === want.tiles[5];
      }, { score, tiles: wantTiles }, { timeout: 30_000 });
    } catch (err) {
      const dump = await page.evaluate(async () => {
        const t = (window as any).__test;
        return {
          mode: t.mode,
          frames: t.machine.state().frames,
          irq: t.machine.state().irqEnable,
          restored: t.hiscoreRestored,
          key: t.hiscoreKey,
          ram: t.readHiscore(),
          tiles: t.hiscoreTiles(),
          idb: await t.loadSavedHiscores(),
        };
      });
      throw new Error(`attract restore did not paint HIGH SCORE: ${JSON.stringify(dump)}\n${err}`);
    }

    const after = await page.evaluate(() => {
      const t = (window as any).__test;
      return { ram: t.readHiscore(), tiles: t.hiscoreTiles() };
    });
    expect(after.ram).toEqual(score);
    expect(after.tiles).toEqual(wantTiles);

    const topLit = await page.evaluate(() => {
      const c = document.getElementById("screen") as HTMLCanvasElement;
      const data = c.getContext("2d")!.getImageData(0, 0, c.width, 24).data;
      let n = 0;
      for (let i = 0; i < data.length; i += 4) {
        if (data[i] || data[i + 1] || data[i + 2]) n++;
      }
      return n;
    });
    expect(topLit, "HIGH SCORE digits should light pixels along the top of the monitor").toBeGreaterThan(20);
  });
});
