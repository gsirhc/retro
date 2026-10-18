import { test, expect } from "./fixtures";
import { bootLive, setPowerSwitch, expectDownload } from "./helpers";

type Page = import("@playwright/test").Page;

const kHddImageBytes = 255974400;
const DOOM = "disk=doom";

async function idbMeta(page: Page, dbName: string): Promise<any> {
  return page.evaluate(async (dbName) => {
    const db: IDBDatabase = await new Promise((resolve, reject) => {
      const req = indexedDB.open(dbName, 1);
      req.onupgradeneeded = () => req.result.createObjectStore("hdd");
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error);
    });
    const val = await new Promise((resolve, reject) => {
      const req = db.transaction("hdd", "readonly").objectStore("hdd").get("c2-meta");
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error);
    });
    db.close();
    return val;
  }, dbName);
}

async function saveHdd(page: Page): Promise<void> {
  await page.evaluate(async () => {
    await (window as any).__test.forcePersistHdd();
    await (window as any).__test.whenHddSaved();
  });
}

async function settle(page: Page): Promise<void> {
  await setPowerSwitch(page, false);
  await page.evaluate(() => (window as any).__test.whenHddSaved());
}

// FAT16 volume label in the first partition's boot sector
async function mountedVolumeLabel(page: Page): Promise<string> {
  return page.evaluate(() => {
    const img: Uint8Array = (window as any).__test.machine.hddImage();
    const lba = img[454] | (img[455] << 8) | (img[456] << 16) | (img[457] << 24);
    return String.fromCharCode(...img.subarray(lba * 512 + 43, lba * 512 + 54)).trim();
  });
}

// Reads a preset image over HTTP and walks its FAT16: root and one game directory, plus
// the text of AUTOEXEC.BAT and named files in that directory.
async function readPreset(page: Page, file: string, dir: string, texts: string[]) {
  return page.evaluate(async ({ file, dir, texts }) => {
    const r = await fetch(file);
    const b = new Uint8Array(await new Response(r.body!.pipeThrough(new DecompressionStream("gzip"))).arrayBuffer());
    const dv = new DataView(b.buffer);
    const part = dv.getUint32(454, true) * 512;
    const bps = dv.getUint16(part + 11, true);
    const spc = b[part + 13];
    const rootEntries = dv.getUint16(part + 17, true);
    const root = part + (dv.getUint16(part + 14, true) + b[part + 16] * dv.getUint16(part + 22, true)) * bps;
    const data = root + rootEntries * 32;
    const entries = (off: number, n: number) => {
      const out: Record<string, { cluster: number; size: number }> = {};
      for (let i = 0; i < n; i++) {
        const e = off + i * 32;
        if (b[e] === 0) break;
        if (b[e] === 0xe5 || b[e + 11] === 0x0f) continue;
        const name = String.fromCharCode(...b.subarray(e, e + 8)).trim();
        const ext = String.fromCharCode(...b.subarray(e + 8, e + 11)).trim();
        out[ext ? name + "." + ext : name] = { cluster: dv.getUint16(e + 26, true), size: dv.getUint32(e + 28, true) };
      }
      return out;
    };
    const cluster = (n: number) => data + (n - 2) * spc * bps;
    const text = (e: { cluster: number; size: number }) =>
      String.fromCharCode(...b.subarray(cluster(e.cluster), cluster(e.cluster) + e.size));
    const top = entries(root, rootEntries);
    const game = entries(cluster(top[dir].cluster), (spc * bps) / 32);
    return {
      len: b.length,
      mbr: b[510] === 0x55 && b[511] === 0xaa,
      fs: String.fromCharCode(...b.subarray(part + 54, part + 62)),
      top: Object.keys(top),
      game: Object.keys(game),
      sizes: Object.fromEntries(Object.entries(game).map(([k, v]) => [k, v.size])),
      autoexec: text(top["AUTOEXEC.BAT"]),
      texts: Object.fromEntries(texts.map((t) => [t, text(game[t])])),
    };
  }, { file, dir, texts });
}

function expectBareFreeDos(r: Awaited<ReturnType<typeof readPreset>>, dir: string) {
  expect(r.len).toBe(kHddImageBytes);
  expect(r.mbr).toBe(true);
  expect(r.fs).toBe("FAT16   ");
  expect(r.top).toEqual(expect.arrayContaining(["KERNEL.SYS", "COMMAND.COM", "CONFIG.SYS", "AUTOEXEC.BAT", "DOS", dir]));
  expect(r.top).not.toContain("FREEDOS");
}

test.describe("preset C: (?disk=doom)", () => {
  test("the DOOM image is a 256MB FAT16 C: with DOOM installed and an AUTOEXEC.BAT that runs it", async ({ page }) => {
    await bootLive(page, { params: DOOM });
    const r = await readPreset(page, "disks/presets/doom-hdd.img.gz", "DOOMS", ["DEFAULT.CFG"]);
    expectBareFreeDos(r, "DOOMS");
    expect(r.game).toEqual(expect.arrayContaining(["DOOM.EXE", "DOOM1.WAD", "SETUP.EXE", "DEFAULT.CFG"]));
    expect(r.game).not.toContain("DOOMS_19.EXE");
    expect(r.sizes["DOOM1.WAD"]).toBe(4196020);
    expect(r.autoexec).toMatch(/CD \\DOOMS\r\nDOOM\r\n$/);
    const defaults = r.texts["DEFAULT.CFG"];
    expect(defaults).toMatch(/snd_sfxdevice\t+3/);
    expect(defaults).toMatch(/snd_sbport\t+544/);
    expect(defaults).toMatch(/screenblocks\t+10\r?\n/);
  });

  test("mounts the DOOM image instead of FreeDOS and relabels the Hard disk panel", async ({ page }) => {
    const gets: string[] = [];
    page.on("request", (r) => { if (r.method() === "GET" && r.url().includes("-hdd.img")) gets.push(r.url()); });
    await bootLive(page, { params: DOOM });
    expect(gets.some((u) => u.includes("disks/presets/doom-hdd.img.gz"))).toBe(true);
    expect(gets.some((u) => u.includes("freedos-hdd.img"))).toBe(false);
    expect(gets.some((u) => new URL(u).pathname.endsWith("-hdd.img"))).toBe(false);
    expect(await mountedVolumeLabel(page)).toBe("DOOM");
    await expect(page.locator("#hddStatus")).toHaveText(/Using: DOOM shareware/);
    await expect(page.locator("#hddResetBtn")).toHaveText("Reset to original DOOM disk");
    await expect(page.locator(".inner h1")).toHaveText("Doom");
    await expect(page.locator(".inner .tagline")).toHaveText("486 DX2-66 Gaming PC");
    await expect(page.locator("#hddBlankBtn")).toBeHidden();
    await expect(page.locator("#hddUploadBtn")).toBeHidden();
    await expect(page.locator("#hddDownloadBtn")).toBeVisible();
  });

  test("saves to its own IndexedDB database and leaves the visitor's own C: alone", async ({ page }) => {
    await bootLive(page);
    await saveHdd(page);
    await settle(page);
    const own = await idbMeta(page, "pc486-hdd");
    expect(own).toMatchObject({ v: 2, modified: true });

    await bootLive(page, { params: DOOM });
    await saveHdd(page);
    await settle(page);
    expect(await idbMeta(page, "pc486-hdd-doom")).toMatchObject({ v: 2, length: kHddImageBytes, modified: true });
    expect(await idbMeta(page, "pc486-hdd")).toEqual(own);

    await page.reload();
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 15_000 });
    await expect(page.locator("#hddStatus")).toHaveText(/saved \(previous visit\)/);
    expect(await mountedVolumeLabel(page)).toBe("DOOM");

    await bootLive(page);
    await expect(page.locator("#hddStatus")).toHaveText(/saved \(previous visit\)/);
    expect(await mountedVolumeLabel(page)).toBe("FREEDOS2022");
  });

  test("Reset to original DOOM disk clears only the DOOM save", async ({ page }) => {
    await bootLive(page);
    await saveHdd(page);
    await settle(page);
    const own = await idbMeta(page, "pc486-hdd");

    await bootLive(page, { params: DOOM });
    await saveHdd(page);
    await setPowerSwitch(page, false);
    await page.locator("#hddResetBtn").click();
    await expect(page.locator("#hddStatus")).toHaveText(/DOOM shareware\. Takes effect at next power-on/);
    await setPowerSwitch(page, true);
    await page.waitForFunction(() => !!(window as any).__test?.machine, null, { timeout: 15_000 });
    await page.evaluate(() => (window as any).__test.whenHddSaved());
    expect(await idbMeta(page, "pc486-hdd-doom")).toMatchObject({ v: 2, modified: false });
    expect(await idbMeta(page, "pc486-hdd")).toEqual(own);
    expect(await mountedVolumeLabel(page)).toBe("DOOM");
  });

  test("Download image names the file after the preset", async ({ page }) => {
    await bootLive(page, { params: DOOM });
    const download = await expectDownload(page, () => page.locator("#hddDownloadBtn").click());
    expect(download.suggestedFilename()).toBe("pc486-doom-hdd.img");
  });

  test("an unknown ?disk= mounts the visitor's own C:", async ({ page }) => {
    await bootLive(page, { params: "disk=nosuchdisk" });
    await expect(page.locator("#hddStatus")).toHaveText(/Using: FreeDOS \(default\)/);
    await expect(page.locator("#hddBlankBtn")).toBeVisible();
    expect(await mountedVolumeLabel(page)).toBe("FREEDOS2022");
    await expect(page.locator("#siteFooter .site-footer-note")).toHaveCount(1);
    await expect(page.locator(".inner h1")).toHaveText("486 DX2-66 Gaming PC");
    await expect(page.locator(".inner .tagline")).toHaveText(/^80486DX2, 66 MHz/);
  });
});

test.describe("preset C: (?disk=wolf3d)", () => {
  test("the Wolfenstein image has Wolf3D installed, its saved view at full width, and mounts with its own label", async ({ page }) => {
    await bootLive(page, { params: "disk=wolf3d" });
    const r = await readPreset(page, "disks/presets/wolf3d-hdd.img.gz", "WOLF1", ["CONFIG.WL1"]);
    expectBareFreeDos(r, "WOLF1");
    expect(r.game).toEqual(expect.arrayContaining(["WOLF3D.EXE", "VSWAP.WL1", "CONFIG.WL1"]));
    expect(r.game).not.toContain("WOLF3D1.EXE");
    expect(r.autoexec).toMatch(/CD \\WOLF1\r\nWOLF3D\r\n$/);
    // WL_MAIN.C WriteConfig: 7 high scores of 66 bytes, then SoundMode, MusicMode, DigiMode ... viewsize at 518
    const cfg = r.texts["CONFIG.WL1"];
    const word = (o: number) => cfg.charCodeAt(o) | (cfg.charCodeAt(o + 1) << 8);
    expect(cfg.length).toBe(522);
    expect([word(462), word(464), word(466)]).toEqual([2, 1, 3]);
    expect(word(518)).toBe(19);
    expect(await mountedVolumeLabel(page)).toBe("WOLF3D");
    await expect(page.locator("#hddStatus")).toHaveText(/Using: Wolfenstein 3-D shareware/);
    await expect(page.locator("#hddResetBtn")).toHaveText("Reset to original Wolfenstein disk");
    await expect(page.locator(".inner h1")).toHaveText("Wolfenstein 3D");
    await expect(page.locator(".inner .tagline")).toHaveText("486 DX2-66 Gaming PC");
  });
});

test.describe("preset page heading", () => {
  test("the game title is in place before app.js runs, so the machine name never flashes", async ({ page }) => {
    await page.route("**/app.js", (route) => route.abort());
    await page.goto("/?disk=duke3d", { waitUntil: "domcontentloaded" });
    expect(await page.evaluate(() => [document.querySelector(".inner h1")!.textContent, document.querySelector(".inner .tagline")!.textContent])).toEqual(["Duke Nukem 3D", "486 DX2-66 Gaming PC"]);
  });
});

test.describe("preset C: (?disk=duke3d)", () => {
  test("the Duke image has the shareware episode installed by 3D Realms' INSTALL, SB16 sound and FM music saved by SETUP, and mounts with its own label", async ({ page }) => {
    await bootLive(page, { params: "disk=duke3d" });
    const r = await readPreset(page, "disks/presets/duke3d-hdd.img.gz", "DUKE3D", ["DUKE3D.CFG"]);
    expectBareFreeDos(r, "DUKE3D");
    expect(r.top).not.toContain("3DDUKE13");
    expect(r.game).toEqual(expect.arrayContaining(["DUKE3D.EXE", "DUKE3D.GRP", "SETUP.EXE", "DUKE3D.CFG"]));
    expect(r.sizes["DUKE3D.GRP"]).toBe(11035779);
    expect(r.autoexec).toMatch(/CD \\DUKE3D\r\nDUKE3D\r\n$/);
    const cfg = r.texts["DUKE3D.CFG"];
    for (const line of ["FXDevice = 0", "MusicDevice = 0", "NumBits = 16", "NumChannels = 2", "MixRate = 22000", "BlasterType = 6", "ControllerType = 1", "ScreenWidth = 320"]) {
      expect(cfg).toContain(line + "\r\n");
    }
    expect(await mountedVolumeLabel(page)).toBe("DUKE3D");
    await expect(page.locator("#hddStatus")).toHaveText(/Using: Duke Nukem 3D shareware/);
    await expect(page.locator("#hddResetBtn")).toHaveText("Reset to original Duke Nukem disk");
    await expect(page.locator(".inner h1")).toHaveText("Duke Nukem 3D");
    await expect(page.locator(".inner .tagline")).toHaveText("486 DX2-66 Gaming PC");
  });
});

test.describe("preset C: (?disk=rott)", () => {
  test("the ROTT image has the shareware episode installed by Apogee's INSTALL, SB16 sound and FM music saved by SNDSETUP, and mounts with its own label", async ({ page }) => {
    await bootLive(page, { params: "disk=rott" });
    const r = await readPreset(page, "disks/presets/rott-hdd.img.gz", "ROTT", ["SOUND.ROT"]);
    expectBareFreeDos(r, "ROTT");
    expect(r.top).not.toContain("1ROTT13");
    expect(r.game).toEqual(expect.arrayContaining(["ROTT.EXE", "HUNTBGIN.WAD", "SETUP.EXE", "SNDSETUP.EXE", "VENDOR.DOC", "SETUP.ROT", "SOUND.ROT"]));
    expect(r.sizes["HUNTBGIN.WAD"]).toBe(6436871);
    expect(r.autoexec).toMatch(/CD \\ROTT\r\nROTT\r\n$/);
    const cfg = r.texts["SOUND.ROT"];
    for (const [key, value] of [["MusicMode", "2"], ["FXMode", "2"], ["NumVoices", "8"], ["NumChannels", "2"], ["NumBits", "16"], ["SBType", "6"], ["SBPort", "$220"], ["SBIrq", "5"], ["SBDma8", "1"], ["SBDma16", "5"]]) {
      expect(cfg).toMatch(new RegExp("^" + key + " +" + value.replace("$", "\\$") + "\r$", "m"));
    }
    expect(await mountedVolumeLabel(page)).toBe("ROTT");
    await expect(page.locator("#hddStatus")).toHaveText(/Using: Rise of the Triad shareware/);
    await expect(page.locator("#hddResetBtn")).toHaveText("Reset to original Rise of the Triad disk");
    await expect(page.locator(".inner h1")).toHaveText("Rise of the Triad");
    await expect(page.locator(".inner .tagline")).toHaveText("486 DX2-66 Gaming PC");
  });
});

test.describe("preset C: (?disk=pinball)", () => {
  test("the Epic Pinball image has Epic's shareware 2.1 directory with SETUP's Sound Blaster driver saved, and mounts with its own label", async ({ page }) => {
    await bootLive(page, { params: "disk=pinball" });
    const r = await readPreset(page, "disks/presets/pinball-hdd.img.gz", "PINBALL", ["SOUNDCRD.INF", "MDRV004R.MUS", "FILE_ID.DIZ"]);
    expectBareFreeDos(r, "PINBALL");
    expect(r.game).toEqual(expect.arrayContaining(["PINBALL.EXE", "EP1.EXE", "SETUP.EXE", "LICENSE.DOC", "VENDOR.DOC", "SOUNDCRD.INF"]));
    expect(r.game).not.toContain("CONFIG.PIN");
    expect(r.game).not.toContain("HISCORE.PIN");
    expect(r.sizes["EP1.EXE"]).toBe(251771);
    expect(r.texts["FILE_ID.DIZ"]).toContain("EPIC PINBALL: Super Android");
    expect(r.autoexec).toMatch(/CD \\PINBALL\r\nPINBALL\r\n$/);
    expect(r.texts["SOUNDCRD.INF"]).toContain("MASISBSERIES");
    expect(r.texts["SOUNDCRD.INF"]).toContain("MDRV004R.MUS");
    const drv = Array.from(r.texts["MDRV004R.MUS"], (c) => c.charCodeAt(0));
    expect(drv[587] | (drv[588] << 8)).toBe(0x220);
    expect(drv[611] | (drv[612] << 8)).toBe(20100);
    expect(await mountedVolumeLabel(page)).toBe("PINBALL");
    await expect(page.locator("#hddStatus")).toHaveText(/Using: Epic Pinball shareware/);
    await expect(page.locator("#hddResetBtn")).toHaveText("Reset to original Epic Pinball disk");
    await expect(page.locator(".inner h1")).toHaveText("Epic Pinball");
  });
});

test.describe("preset C: (?disk=tyrian)", () => {
  test("the Tyrian image has Tyrian 2.1 with FM music, Sound Blaster effects and 16-bar volumes, and mounts with its own label", async ({ page }) => {
    await bootLive(page, { params: "disk=tyrian" });
    const r = await readPreset(page, "disks/presets/tyrian-hdd.img.gz", "TYRIAN", ["TYRIAN.CFG"]);
    expectBareFreeDos(r, "TYRIAN");
    expect(r.game).toEqual(expect.arrayContaining(["TYRIAN.EXE", "FILE0001.EXE", "RTM.EXE", "DPMI16BI.OVL", "TYRIAN.CFG", "TYRIAN.SAV"]));
    expect(r.autoexec).toMatch(/CD \\TYRIAN\r\nTYRIAN\r\n$/);
    const cfg = Array.from(r.texts["TYRIAN.CFG"], (c) => c.charCodeAt(0));
    expect(cfg).toEqual([0x01, 0x04, 0x00, 0x01, 0x00, 0x03, 0x02, 0x02, 0x00, 0x00, 0x01, 0x04, 0x05, 0x05, 0xc0, 0x00, 0xc8, 0x00, 0x00, 0x00, 0x48, 0x50, 0x4b, 0x4d, 0x39, 0x1c, 0x1d, 0x38]);
    expect(await mountedVolumeLabel(page)).toBe("TYRIAN");
    await expect(page.locator("#hddStatus")).toHaveText(/Using: Tyrian 2\.1 freeware/);
    await expect(page.locator("#hddResetBtn")).toHaveText("Reset to original Tyrian disk");
    await expect(page.locator(".inner h1")).toHaveText("Tyrian");
    await expect(page.locator(".inner .tagline")).toHaveText("486 DX2-66 Gaming PC");
    const note = page.locator("#siteFooter .site-footer-note");
    await expect(note).toContainText("All shareware and freeware titles remain the property of their respective owners");
    await expect(note.locator("a")).toHaveAttribute("href", "../about.html#rights");
  });
});
