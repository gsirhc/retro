import { test, expect } from "@playwright/test";


async function chooseTheme(page, theme, mode = "light") {
  await page.locator("#pageThemeBtn").click();
  await page.locator(`#themeDialog input[name="pageThemeFamily"][value="${theme}"]`).check();
  await page.locator(`#themeDialog input[name="pageThemeMode"][value="${mode}"]`).check();
  await page.locator("#themeDialogDone").click();
}


// The retroweb/ landing page (:8410). It shares the `retro8080.theme` localStorage key with the
// emulator, so a theme picked here carries into /pc486/.

const HOME = "http://localhost:8410/";

test.describe("retroweb landing page", () => {
  test("lists the 486 DX2-66 Gaming PC with a launch link to /pc486/", async ({ page }) => {
    await page.goto(HOME);
    await expect(page).toHaveTitle(/Welcome to OAC Emulators/);

    const card = page.locator('a.machine-card[href="pc486/"]');
    await expect(card).toHaveAttribute("href", "pc486/");
    await expect(card.locator(".name")).toHaveText(/486 DX2-66/i);

    const shot = card.locator("img.shot");
    await expect(shot).toHaveAttribute("src", /assets\/pc486-panel\.jpg$/);
    await expect(shot).toHaveAttribute("width", "286");
    await expect(shot).toHaveAttribute("height", "128");
    await expect(shot).toHaveJSProperty("complete", true);
    expect(await shot.evaluate((img: HTMLImageElement) => img.naturalWidth)).toBe(286);
    expect(await shot.evaluate((img: HTMLImageElement) => img.naturalHeight)).toBe(128);
  });

  test("Ready to Play lists DOOM, Wolfenstein 3-D and Tyrian, each linking to its pc486 preset", async ({ page }) => {
    await page.goto(HOME);
    await expect(page.locator("h2.section-heading")).toHaveText([
      "Systems",
      "Ready to Play",
      "Arcade",
      "Homebrew",
    ]);
    const card = page.locator('a.machine-card[href="pc486/?disk=doom"]');
    await expect(card.locator(".name")).toHaveText("Doom");
    await expect(card.locator(".meta")).toHaveText("486 DX2-66 Gaming PC");

    const shot = card.locator("img.shot");
    await expect(shot).toHaveAttribute("src", /assets\/doom-e1m1\.jpg$/);
    await expect(shot).toHaveJSProperty("complete", true);
    expect(await shot.evaluate((img: HTMLImageElement) => img.naturalWidth)).toBe(286);
    expect(await shot.evaluate((img: HTMLImageElement) => img.naturalHeight)).toBe(128);

    const wolf = page.locator('a.machine-card[href="pc486/?disk=wolf3d"]');
    await expect(wolf.locator(".name")).toHaveText("Wolfenstein 3D");
    await expect(wolf.locator(".meta")).toHaveText("486 DX2-66 Gaming PC");
    await expect(wolf.locator("img.shot")).toHaveJSProperty("naturalWidth", 286);

    const tyrian = page.locator('a.machine-card[href="pc486/?disk=tyrian"]');
    await expect(tyrian.locator(".name")).toHaveText("Tyrian");
    await expect(tyrian.locator(".meta")).toHaveText("486 DX2-66 Gaming PC");
    await expect(tyrian.locator("img.shot")).toHaveJSProperty("naturalWidth", 286);
    await expect(page.locator(".ready-note a")).toHaveText(["libretro", "More info"]);
    await expect(page.locator(".ready-note a").first()).toHaveAttribute("href", "https://www.libretro.com/");
    await expect(page.locator(".ready-note a").last()).toHaveAttribute("href", "about.html#rights");
  });

  test("the theme selector switches the page and persists to retro8080.theme", async ({ page }) => {
    await page.goto(HOME);
    const root = page.locator("html");
    await expect(root).toHaveAttribute("data-theme", "win");

    await chooseTheme(page, "modern");
    await expect(root).toHaveAttribute("data-theme", "modern");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe("modern");

    await chooseTheme(page, "web94");
    await expect(root).toHaveAttribute("data-theme", "web94");
    expect(await page.evaluate(() => localStorage.getItem("retro8080.theme"))).toBe("web94");
  });

  test("a stored retro8080.theme is honoured on load", async ({ page }) => {
    await page.goto(HOME);
    await page.evaluate(() => localStorage.setItem("retro8080.theme", "modern"));
    await page.reload();
    await expect(page.locator("html")).toHaveAttribute("data-theme", "modern");
    await expect(page.locator("#pageThemeBtn")).toHaveText("Modern");
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

test.describe("Ready to Play thumbnails", () => {
  const DOS = "https://thumbnails.libretro.com/DOS/";
  const DIRS = { snap: "Named_Snaps", title: "Named_Titles", boxart: "Named_Boxarts" };
  const ART: Record<string, string> = {
    "pc486/?disk=doom": "DOOM%20(1993).png",
    "pc486/?disk=wolf3d": "Wolfenstein%203D%20(1992).png",
    "ibmpc-at/?disk=keen1": "Commander%20Keen%201%20-%20Marooned%20on%20Mars%20(1990).png",
    "pc486/?disk=tyrian": "Tyrian%20(1995).png",
  };
  const PNG = Buffer.from("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGNgYGAAAAAEAAH2FzhVAAAAAElFTkSuQmCC", "base64");

  async function stub(page, fail: string[] = []) {
    const hits: string[] = [];
    await page.route("https://thumbnails.libretro.com/**", (route) => {
      const url = route.request().url();
      hits.push(url);
      if (fail.some((f) => url.endsWith(f))) return route.abort();
      return route.fulfill({ status: 200, contentType: "image/png", body: PNG });
    });
    return hits;
  }

  const fit = (page, href: string) => page.locator(`a.machine-card[href="${href}"] img.shot`).evaluate((img) => getComputedStyle(img).objectFit + " " + getComputedStyle(img).objectPosition);

  test("Original by default and fetches nothing from libretro", async ({ page }) => {
    const hits = await stub(page);
    await page.goto(HOME);
    await expect(page.locator("#readyArt")).toHaveValue("");
    await expect(page.locator("#readyArt option")).toHaveText(["Original", "Screenshot", "Title screen", "Box art"]);
    await expect(page.locator("a.machine-card.art")).toHaveCount(0);
    await expect(page.locator('a.machine-card[href="pc486/?disk=doom"] img.shot')).toHaveAttribute("src", "assets/doom-e1m1.jpg");
    expect(hits).toEqual([]);
  });

  for (const [mode, dir] of Object.entries(DIRS)) {
    test(`${mode} loads each title's libretro DOS ${dir} image`, async ({ page }) => {
      const hits = await stub(page);
      await page.goto(HOME);
      await page.locator("#readyArt").selectOption(mode);
      for (const [href, file] of Object.entries(ART)) {
        const card = page.locator(`a.machine-card[href="${href}"]`);
        await expect(card).toHaveClass(/\bart\b/);
        const shot = card.locator("img.shot");
        await expect(shot).toHaveAttribute("src", DOS + dir + "/" + file);
        await expect(shot).toHaveJSProperty("complete", true);
        await expect(shot).toHaveAttribute("referrerpolicy", "no-referrer");
        expect(await fit(page, href)).toBe(mode === "snap" ? "cover 50% 50%" : "cover 50% 0%");
      }
      await expect.poll(() => new Set(hits).size).toBe(4);
      expect(hits.every((h) => h.startsWith(DOS + dir + "/"))).toBe(true);
      expect(await page.evaluate(() => localStorage.getItem("retroweb.readyArt"))).toBe(mode);
      expect(await page.evaluate(() => localStorage.getItem("retroweb.arcadeArt"))).toBeNull();
    });
  }

  test("a saved style hides the original tiles until the artwork arrives, so they never flash", async ({ page }) => {
    let release: () => void = () => {};
    const gate = new Promise<void>((r) => { release = r; });
    await page.route("https://thumbnails.libretro.com/**", async (route) => {
      await gate;
      await route.fulfill({ status: 200, contentType: "image/png", body: PNG });
    });
    await page.goto(HOME);
    await expect(page.locator("html")).not.toHaveClass(/art-pending/);
    await page.evaluate(() => localStorage.setItem("retroweb.readyArt", "title"));
    await page.reload({ waitUntil: "domcontentloaded" });
    await expect(page.locator("html")).toHaveClass(/art-pending/);
    const shot = page.locator('a.machine-card[href="pc486/?disk=doom"] img.shot');
    await expect(shot).toHaveAttribute("src", /^https:\/\/thumbnails\.libretro\.com\//);
    expect(await shot.evaluate((img) => getComputedStyle(img).visibility)).toBe("hidden");
    release();
    await expect(shot).toHaveClass(/art-ready/);
    expect(await shot.evaluate((img) => getComputedStyle(img).visibility)).toBe("visible");
  });

  test("choice persists across reload and Original restores the emulator captures", async ({ page }) => {
    const hits = await stub(page);
    await page.goto(HOME);
    await page.locator("#readyArt").selectOption("title");
    await page.reload();
    await expect(page.locator("#readyArt")).toHaveValue("title");
    await expect(page.locator('a.machine-card[href="pc486/?disk=wolf3d"] img.shot')).toHaveAttribute("src", DOS + "Named_Titles/" + ART["pc486/?disk=wolf3d"]);

    const before = hits.length;
    await page.locator("#readyArt").selectOption("");
    await expect(page.locator("a.machine-card.art")).toHaveCount(0);
    await expect(page.locator('a.machine-card[href="pc486/?disk=tyrian"] img.shot')).toHaveAttribute("src", "assets/tyrian-demo.jpg");
    expect(await fit(page, "pc486/?disk=tyrian")).toBe("cover 0% 50%");
    expect(hits.length).toBe(before);
  });

  test("an image that fails to load falls back to that title's emulator capture", async ({ page }) => {
    await stub(page, ["Named_Boxarts/Tyrian%20(1995).png"]);
    await page.goto(HOME);
    await page.locator("#readyArt").selectOption("boxart");
    const tyrian = page.locator('a.machine-card[href="pc486/?disk=tyrian"]');
    await expect(tyrian.locator("img.shot")).toHaveAttribute("src", "assets/tyrian-demo.jpg");
    await expect(tyrian).not.toHaveClass(/\bart\b/);
    expect(await fit(page, "pc486/?disk=tyrian")).toBe("cover 0% 50%");
    await expect(page.locator("a.machine-card.art")).toHaveCount(3);
  });
});
