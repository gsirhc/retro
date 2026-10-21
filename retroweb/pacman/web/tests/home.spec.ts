import { test, expect } from "@playwright/test";


async function chooseTheme(page, theme, mode = "light") {
  await page.locator("#pageThemeBtn").click();
  await page.locator(`#themeDialog input[name="pageThemeFamily"][value="${theme}"]`).check();
  await page.locator(`#themeDialog input[name="pageThemeMode"][value="${mode}"]`).check();
  await page.locator("#themeDialogDone").click();
}


// Landing page on :8510. Shares the retro8080.theme key with the emulator.

const HOME = "http://localhost:8510/";
const CATALOG = HOME + "super_secret.html";
const ARCADE = "http://localhost:8510/arcade/";

test.describe("retroweb landing page", () => {
  test("Arcade section is a full-width Enter the Arcade portal; cabinets stay on the hub", async ({ page }) => {
    await page.goto(CATALOG);
    await expect(page).toHaveTitle(/Welcome to OAC Emulators/);
    await expect(page.locator("h2.section-heading")).toHaveText([
      "Systems",
      "Ready to Play",
      "Arcade",
      "Homebrew",
    ]);

    await expect(page.locator('a.machine-card[href="pacman/"]')).toHaveCount(0);
    await expect(page.locator('a.machine-card[href="galaga/"]')).toHaveCount(0);
    await expect(page.locator('a.machine-card[href="frogger/"]')).toHaveCount(0);

    const portal = page.locator("a.arcade-portal");
    await expect(portal).toHaveAttribute("href", "arcade/");
    await expect(portal).toHaveAttribute("aria-label", "Enter the Arcade");
    await expect(portal.locator(".portal-neon")).toHaveText("Enter the Arcade");
    await portal.click();
    await expect(page).toHaveURL(/\/arcade\/?$/);
    await expect(page).toHaveTitle(/Arcade/);
  });

  test("the theme selector switches the page and persists to retro8080.theme", async ({ page }) => {
    await page.goto(HOME);
    const root = page.locator("html");
    await expect(root).toHaveAttribute("data-theme", "win");
    await expect(page.locator("#siteFooter a.about-sign")).toHaveCount(0);

    await chooseTheme(page, "modern");
    await expect(root).toHaveAttribute("data-theme", "modern");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe("modern");
    await expect(page.locator("#siteFooter a.about-sign")).toHaveCount(0);
  });

  test("the public footer has no Help control, and about.html still returns home", async ({ page }) => {
    await page.goto(HOME);
    const footer = page.locator("#siteFooter");
    await expect(footer).toContainText(/© 2026 oac-emu\.dev/);
    await expect(footer).toContainText(/old-ass computer/);
    await expect(footer.locator("a.about-sign")).toHaveCount(0);
    await page.goto(HOME + "about.html");
    await expect(page).toHaveURL(/\/about\.html$/);
    await expect(page).toHaveTitle(/About/i);
    await expect(page.locator("#siteFooter a.about-sign")).toHaveCount(0);
    await expect(page.locator("a.pb-close")).toHaveAttribute("href", "./");
    await page.locator("a.pb-close").click();
    await expect(page).toHaveTitle(/Welcome to OAC Emulators/);
  });

  test("Windows title-bar buttons: soft-minimize collapses the page; close is disabled", async ({ page }) => {
    await page.goto(HOME);
    await expect(page.locator(".pb-close.is-disabled")).toHaveAttribute("aria-disabled", "true");
    await expect(page.locator("a.pb-close")).toHaveCount(0);
    await expect(page.locator(".pb-max")).toBeDisabled();
    await expect(page.locator(".inner")).toBeVisible();
    await page.locator(".pb-min").click();
    await expect(page.locator(".page")).toHaveClass(/is-minimized/);
    await expect(page.locator(".inner")).toBeHidden();
    await expect(page.locator("#siteFooter")).toBeHidden();
    await page.locator(".pb-min").click();
    await expect(page.locator(".page")).not.toHaveClass(/is-minimized/);
    await expect(page.locator(".inner")).toBeVisible();
  });

});

test.describe("arcade hub", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto(ARCADE);
    await page.evaluate(() => localStorage.removeItem("retroweb.arcadeSort"));
    await page.reload();
  });

  test("lists eleven cabinets in Name order by default, including Pac-Man and Ms. Pac-Man", async ({ page }) => {
    await expect(page).toHaveTitle(/Arcade/);
    await expect(page.locator(".tagline a")).toHaveText("More info");
    await expect(page.locator(".tagline a")).toHaveAttribute("href", "../about.html#rights");
    await expect(page.locator("#arcadeCatalog > h2.section-heading")).toHaveCount(0);
    const names = await page.locator("#arcadeCatalog a.machine-card .name").allTextContents();
    expect(names).toEqual([
      "Asteroids",
      "Battlezone",
      "Centipede",
      "Frogger",
      "Galaga",
      "Galaxian",
      "Lunar Rescue",
      "Missile Command",
      "Ms. Pac-Man",
      "Pac-Man",
      "Scramble",
      "Space Invaders",
    ]);
    const pac = page.locator('a.machine-card[href="../pacman/"]');
    await expect(pac.locator(".name")).toHaveText("Pac-Man");
    const shot = pac.locator("img.shot");
    await expect(shot).toHaveAttribute("src", /assets\/pacman-cabinet\.png$/);
    await expect(shot).toHaveJSProperty("complete", true);
    expect(await shot.evaluate((img: HTMLImageElement) => img.naturalWidth)).toBe(286);
    expect(await shot.evaluate((img: HTMLImageElement) => img.naturalHeight)).toBe(128);

    const ms = page.locator('a.machine-card[href="../pacman/?game=mspacman"]');
    await expect(ms.locator(".name")).toHaveText("Ms. Pac-Man");
  });

  test("Sort by Year / CPU / Manufacturer groups with section headings and persists", async ({ page }) => {
    const sel = page.locator("#arcadeSort");
    await expect(sel).toHaveValue("name");

    await sel.selectOption("year");
    expect(await page.evaluate(() => localStorage.getItem("retroweb.arcadeSort"))).toBe("year");
    await expect(page.locator("#arcadeCatalog > h2.section-heading")).toHaveText([
      "1978", "1979", "1980", "1981", "1982",
    ]);
    let names = await page.locator("#arcadeCatalog a.machine-card .name").allTextContents();
    expect(names).toEqual([
      "Space Invaders",
      "Asteroids",
      "Galaxian",
      "Lunar Rescue",
      "Battlezone",
      "Missile Command",
      "Pac-Man",
      "Centipede",
      "Frogger",
      "Galaga",
      "Scramble",
      "Ms. Pac-Man",
    ]);

    await sel.selectOption("cpu");
    await expect(page.locator("#arcadeCatalog > h2.section-heading")).toHaveText(["6502", "8080", "Z80"]);
    names = await page.locator("#arcadeCatalog a.machine-card .name").allTextContents();
    expect(names.slice(0, 4)).toEqual(["Asteroids", "Battlezone", "Centipede", "Missile Command"]);
    expect(names.slice(4, 6)).toEqual(["Lunar Rescue", "Space Invaders"]);
    expect(names.slice(6)).toEqual([
      "Frogger",
      "Galaga",
      "Galaxian",
      "Ms. Pac-Man",
      "Pac-Man",
      "Scramble",
    ]);

    await sel.selectOption("manufacturer");
    await expect(page.locator("#arcadeCatalog > h2.section-heading")).toHaveText([
      "Atari", "Konami", "Midway", "Namco", "Taito",
    ]);
    names = await page.locator("#arcadeCatalog a.machine-card .name").allTextContents();
    expect(names).toEqual([
      "Asteroids",
      "Battlezone",
      "Centipede",
      "Missile Command",
      "Frogger",
      "Scramble",
      "Galaga",
      "Ms. Pac-Man",
      "Space Invaders",
      "Galaxian",
      "Pac-Man",
      "Lunar Rescue",
    ]);

    await page.reload();
    await expect(page.locator("#arcadeSort")).toHaveValue("manufacturer");
    await expect(page.locator("#arcadeCatalog > h2.section-heading")).toHaveText([
      "Atari", "Konami", "Midway", "Namco", "Taito",
    ]);
    names = await page.locator("#arcadeCatalog a.machine-card .name").allTextContents();
    expect(names[0]).toBe("Asteroids");
    expect(names[names.length - 1]).toBe("Lunar Rescue");
  });
});

test.describe("arcade hub real art", () => {
  const LIBRETRO = "https://thumbnails.libretro.com/MAME/";
  const DIRS = { snap: "Named_Snaps", title: "Named_Titles", boxart: "Named_Boxarts" };
  const ART: Record<string, string> = {
    "../asteroids/": "Asteroids%20(rev%204).png",
    "../battlezone/": "Battle%20Zone%20(rev%202).png",
    "../centipede/": "Centipede%20(revision%204).png",
    "../frogger/": "Frogger.png",
    "../galaga/": "Galaga%20(Midway%20set%201).png",
    "../galaxian/": "Galaxian%20(Namco%20set%201).png",
    "../missile/": "Missile%20Command%20(rev%203).png",
    "../pacman/?game=mspacman": "Ms.%20Pac-Man.png",
    "../pacman/": "Pac-Man%20(Midway).png",
    "../scramble/": "Scramble.png",
    "../invaders/": "Space%20Invaders%20_%20Space%20Invaders%20M.png",
    "../lunarrescue/": "Lunar%20Rescue.png",
  };
  const PNG = Buffer.from("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGNgYGAAAAAEAAH2FzhVAAAAAElFTkSuQmCC", "base64");

  async function stub(page, fail: string[] = []) {
    const hits: string[] = [];
    const handler = (route) => {
      const url = route.request().url();
      hits.push(url);
      if (fail.some((f) => url.endsWith(f))) return route.abort();
      return route.fulfill({ status: 200, contentType: "image/png", body: PNG });
    };
    await page.route("https://thumbnails.libretro.com/**", handler);
    await page.route("https://adb.arcadeitalia.net/**", handler);
    return hits;
  }

  test("Original by default and fetches nothing from libretro", async ({ page }) => {
    const hits = await stub(page);
    await page.goto(ARCADE);
    await expect(page.locator("#arcadeArt")).toHaveValue("");
    await expect(page.locator("#arcadeArt option")).toHaveText(["Original", "Screenshot", "Title screen", "Box art", "Marquee"]);
    await expect(page.locator("a.machine-card.art")).toHaveCount(0);
    for (const href of Object.keys(ART)) {
      await expect(page.locator(`a.machine-card[href="${href}"] img.shot`)).toHaveAttribute("src", /^\.\.\/assets\/[a-z]+-cabinet\.png$/);
    }
    expect(hits).toEqual([]);
  });

  for (const [mode, dir] of Object.entries(DIRS)) {
    test(`${mode} loads each cabinet's libretro ${dir} image`, async ({ page }) => {
      const hits = await stub(page);
      await page.goto(ARCADE);
      await page.locator("#arcadeArt").selectOption(mode);
      for (const [href, file] of Object.entries(ART)) {
        const card = page.locator(`a.machine-card[href="${href}"]`);
        await expect(card).toHaveClass(/\bart\b/);
        const shot = card.locator("img.shot");
        await expect(shot).toHaveAttribute("src", LIBRETRO + dir + "/" + file);
        await expect(shot).toHaveJSProperty("complete", true);
        await expect(shot).toHaveAttribute("referrerpolicy", "no-referrer");
      }
      await expect.poll(() => new Set(hits).size).toBe(12);
      expect(hits.every((h) => h.startsWith(LIBRETRO + dir + "/"))).toBe(true);
      expect(await page.evaluate(() => localStorage.getItem("retroweb.arcadeArt"))).toBe(mode);
    });
  }

  test("marquee loads each cabinet's Arcade Database marquee fitted whole", async ({ page }) => {
    const ADB = "https://adb.arcadeitalia.net/media/mame.current/marquees/";
    const SETS: Record<string, string> = {
      "../asteroids/": "asteroid", "../battlezone/": "bzone", "../centipede/": "centiped", "../frogger/": "frogger",
      "../galaga/": "galaga", "../galaxian/": "galaxian", "../missile/": "missile",
      "../pacman/?game=mspacman": "mspacman", "../pacman/": "pacman", "../scramble/": "scramble",
      "../invaders/": "invaders", "../lunarrescue/": "lrescue",
    };
    const hits = await stub(page);
    await page.goto(ARCADE);
    await page.locator("#arcadeArt").selectOption("marquee");
    for (const [href, set] of Object.entries(SETS)) {
      const card = page.locator(`a.machine-card[href="${href}"]`);
      await expect(card).toHaveClass(/\bart\b.*\bmarquee\b|\bmarquee\b.*\bart\b/);
      const shot = card.locator("img.shot");
      await expect(shot).toHaveAttribute("src", ADB + set + ".png");
      await expect(shot).toHaveJSProperty("complete", true);
      expect(await shot.evaluate((img) => getComputedStyle(img).objectFit + " " + getComputedStyle(img).objectPosition)).toBe("contain 50% 50%");
    }
    await expect.poll(() => new Set(hits).size).toBe(12);
    expect(hits.every((h) => h.startsWith(ADB))).toBe(true);

    await page.locator("#arcadeArt").selectOption("snap");
    await expect(page.locator("a.machine-card.marquee")).toHaveCount(0);
    await expect(page.locator("a.machine-card.art")).toHaveCount(12);
  });

  test("a saved style hides the original tiles until the artwork arrives, so they never flash", async ({ page }) => {
    let release: () => void = () => {};
    const gate = new Promise<void>((r) => { release = r; });
    await page.route("https://thumbnails.libretro.com/**", async (route) => {
      await gate;
      await route.fulfill({ status: 200, contentType: "image/png", body: PNG });
    });
    await page.goto(ARCADE);
    await expect(page.locator("html")).not.toHaveClass(/art-pending/);
    await page.evaluate(() => localStorage.setItem("retroweb.arcadeArt", "title"));
    await page.reload({ waitUntil: "domcontentloaded" });
    await expect(page.locator("html")).toHaveClass(/art-pending/);
    const shot = page.locator('a.machine-card[href="../frogger/"] img.shot');
    await expect(shot).toHaveAttribute("src", /^https:\/\/thumbnails\.libretro\.com\//);
    expect(await shot.evaluate((img) => getComputedStyle(img).visibility)).toBe("hidden");
    release();
    await expect(shot).toHaveClass(/art-ready/);
    expect(await shot.evaluate((img) => getComputedStyle(img).visibility)).toBe("visible");
  });

  test("choice persists across reload, survives sorting, and Original restores the starfield tiles", async ({ page }) => {
    const hits = await stub(page);
    await page.goto(ARCADE);
    await page.locator("#arcadeArt").selectOption("boxart");
    await page.reload();
    await expect(page.locator("#arcadeArt")).toHaveValue("boxart");
    await expect(page.locator('a.machine-card[href="../frogger/"] img.shot')).toHaveAttribute("src", LIBRETRO + "Named_Boxarts/Frogger.png");
    const pos = (href: string) => page.locator(`a.machine-card[href="${href}"] img.shot`).evaluate((img) => getComputedStyle(img).objectFit + " " + getComputedStyle(img).objectPosition);
    expect(await pos("../pacman/")).toBe("cover 50% 100%");
    expect(await pos("../asteroids/")).toBe("cover 50% 50%");
    expect(await pos("../galaga/")).toBe("cover 50% 0%");

    await page.locator("#arcadeSort").selectOption("year");
    await expect(page.locator('a.machine-card[href="../galaga/"] img.shot')).toHaveAttribute("src", LIBRETRO + "Named_Boxarts/" + ART["../galaga/"]);

    await page.locator("#arcadeArt").selectOption("title");
    await expect(page.locator('a.machine-card[href="../galaga/"] img.shot')).toHaveAttribute("src", LIBRETRO + "Named_Titles/" + ART["../galaga/"]);
    expect(await pos("../pacman/")).toBe("cover 50% 50%");

    const before = hits.length;
    await page.locator("#arcadeArt").selectOption("");
    await expect(page.locator("a.machine-card.art")).toHaveCount(0);
    await expect(page.locator('a.machine-card[href="../pacman/"] img.shot')).toHaveAttribute("src", "../assets/pacman-cabinet.png");
    await expect(page.locator("a.machine-card img.shot[src^='../assets/']")).toHaveCount(12);
    expect(await pos("../pacman/")).toBe("cover 0% 50%");
    expect(await page.evaluate(() => localStorage.getItem("retroweb.arcadeArt"))).toBe("");
    expect(hits.length).toBe(before);
  });

  test("an image that fails to load falls back to that cabinet's starfield tile", async ({ page }) => {
    await stub(page, ["Named_Boxarts/Scramble.png"]);
    await page.goto(ARCADE);
    await page.locator("#arcadeArt").selectOption("boxart");
    const scramble = page.locator('a.machine-card[href="../scramble/"]');
    await expect(scramble.locator("img.shot")).toHaveAttribute("src", "../assets/scramble-cabinet.png");
    await expect(scramble).not.toHaveClass(/\bart\b/);
    expect(await scramble.locator("img.shot").evaluate((img) => getComputedStyle(img).objectPosition)).toBe("0% 50%");
    await expect(page.locator('a.machine-card[href="../frogger/"] img.shot')).toHaveAttribute("src", LIBRETRO + "Named_Boxarts/Frogger.png");
    await expect(page.locator("a.machine-card.art")).toHaveCount(11);
  });
});
