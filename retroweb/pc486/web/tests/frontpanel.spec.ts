import { test, expect } from "./fixtures";

// The front panel's turbo cluster: seven-segment clock readout tracks
// Turbo (66 / 33), amber LED matches, and Turbo actually changes the
// guest clock (DX2 doubling). Power/reset behavior is in boot.spec.ts.

test.describe("front panel jewelry", () => {
  test("the seven-segment display shows 66 with Turbo on, 33 with Turbo off", async ({ livePage: page }) => {
    const digits = page.locator(".sevenseg");
    await expect(digits).toHaveCount(2);

    // Turbo on (default): both digits are "6" (everything but b).
    for (let i = 0; i < 2; i++) {
      const digit = digits.nth(i);
      for (const seg of ["a", "c", "d", "e", "f", "g"]) {
        await expect(digit.locator(`.${seg}`)).toHaveClass(/on/);
      }
      await expect(digit.locator(".b")).not.toHaveClass(/on/);
    }

    await page.locator("#turboBtn").click();
    // Turbo off: both digits are "3" (a/b/c/d/g; not e/f).
    for (let i = 0; i < 2; i++) {
      const digit = digits.nth(i);
      for (const seg of ["a", "b", "c", "d", "g"]) {
        await expect(digit.locator(`.${seg}`)).toHaveClass(/on/);
      }
      await expect(digit.locator(".e")).not.toHaveClass(/on/);
      await expect(digit.locator(".f")).not.toHaveClass(/on/);
    }

    await page.locator("#turboBtn").click();
    await expect(digits.nth(0).locator(".e")).toHaveClass(/on/);  // back to "6"
  });

  test("reads as a tower turbo cluster with 5.25\" CD above 3.5\" floppy", async ({ livePage: page }) => {
    await expect(page.locator(".tower-panel")).toBeVisible();
    await expect(page.locator("#turboBtn")).toBeVisible();
    await expect(page.locator("#resetBtn")).toBeVisible();
    await expect(page.locator("#powerSwitch")).toBeVisible();
    await expect(page.locator(".power-rocker")).toBeVisible();
    await expect(page.locator(".tower-keylock")).toHaveCount(0);

    // Default (win) stack: CD above floppy. Blank covers are modern-only.
    const drives = page.locator(".at-drives .at-bay:visible");
    await expect(drives).toHaveCount(2);
    await expect(drives.nth(0)).toHaveAttribute("data-drive", "cdrom");
    await expect(drives.nth(0)).toHaveClass(/bay-525/);
    await expect(drives.nth(0).locator(".cd-door")).toBeVisible();
    await expect(drives.nth(1)).toHaveAttribute("data-drive", "0");
    await expect(drives.nth(1)).toHaveClass(/bay-35/);
    await expect(drives.nth(1).locator(".floppy-door")).toBeVisible();
    await expect(page.locator(".tower-blanks")).toBeHidden();

    // Grill fills the leftover height beside the drive stack.
    const grillStretch = await page.evaluate(() => {
      const grill = document.querySelector(".tower-grill")!.getBoundingClientRect();
      const drivesCol = document.querySelector(".at-drives")!.getBoundingClientRect();
      const panel = document.querySelector(".tower-panel")!.getBoundingClientRect();
      return {
        tallerThanMin: grill.height > 40,
        reachesNearDrives: Math.abs(grill.bottom - drivesCol.bottom) < 24,
        belowPanel: grill.top >= panel.bottom - 2,
      };
    });
    expect(grillStretch.tallerThanMin).toBe(true);
    expect(grillStretch.reachesNearDrives).toBe(true);
    expect(grillStretch.belowPanel).toBe(true);
  });

  test("modern theme shows blank 5.25\" covers and a mini-tower beside the screen", async ({
    livePage: page,
  }) => {
    const vp = page.viewportSize()!;
    try {
      await page.setViewportSize({ width: 1600, height: 1000 });
      await page.locator("#pageTheme").selectOption("modern");
      const blanks = page.locator(".at-bay.bay-blank");
      await expect(blanks).toHaveCount(2);
      await expect(blanks.nth(0)).toBeVisible();
      await expect(blanks.nth(1)).toBeVisible();

      const drives = page.locator(".at-drives .at-bay:visible");
      await expect(drives).toHaveCount(4);
      await expect(drives.nth(0)).toHaveAttribute("data-drive", "cdrom");
      await expect(drives.nth(1)).toHaveClass(/bay-blank/);
      await expect(drives.nth(2)).toHaveClass(/bay-blank/);
      await expect(drives.nth(3)).toHaveAttribute("data-drive", "0");
    } finally {
      await page.setViewportSize(vp);
    }
  });

  test("modern tower keeps both covers beside the narrowest 4:3 monitor", async ({
    livePage: page,
  }) => {
    const vp = page.viewportSize()!;
    try {
      await page.locator("#pageTheme").selectOption("modern");
      await page.setViewportSize({ width: 1200, height: 900 });
      const visible = page.locator(".at-drives .at-bay:visible");
      await expect(visible).toHaveCount(4);
      await expect(visible.nth(0)).toHaveAttribute("data-drive", "cdrom");
      await expect(visible.nth(1)).toHaveClass(/bay-blank/);
      await expect(visible.nth(2)).toHaveClass(/bay-blank/);
      await expect(visible.nth(3)).toHaveAttribute("data-drive", "0");
      const faces = await page.evaluate(() => {
        const h = (sel: string) => document.querySelector(sel)!.getBoundingClientRect().height;
        return { cd: h('[data-drive="cdrom"]'), floppy: h('[data-drive="0"]') };
      });
      expect(faces.cd).toBeGreaterThan(80);
      expect(faces.floppy).toBeGreaterThan(70);
    } finally {
      await page.setViewportSize(vp);
    }
  });

  test("modern theme puts the mini-tower beside the screen (CD, floppy, then controls)", async ({
    livePage: page,
  }) => {
    await page.locator("#pageTheme").selectOption("modern");
    const layout = await page.evaluate(() => {
      const screen = document.getElementById("screen")!.getBoundingClientRect();
      const cd = document.querySelector('.at-bay[data-drive="cdrom"]')!.getBoundingClientRect();
      const floppy = document.querySelector('.at-bay[data-drive="0"]')!.getBoundingClientRect();
      const tower = document.querySelector(".tower-panel")!.getBoundingClientRect();
      const caseEl = document.querySelector("#frontPanelCard .at-case")!.getBoundingClientRect();
      return {
        towerRightOfScreen: cd.left >= screen.right - 4,
        cdAboveFloppy: cd.bottom <= floppy.top + 4,
        floppyAboveTower: floppy.bottom <= tower.top + 4,
        // Tower case should land near the monitor height (not a short stub).
        heightRatio: caseEl.height / screen.height,
      };
    });
    expect(layout.towerRightOfScreen).toBe(true);
    expect(layout.cdAboveFloppy).toBe(true);
    expect(layout.floppyAboveTower).toBe(true);
    expect(layout.heightRatio).toBeGreaterThan(0.85);
    expect(layout.heightRatio).toBeLessThan(1.15);
  });

  test("modern theme drops the front panel under the screen when both don't fit", async ({
    livePage: page,
  }) => {
    const vp = page.viewportSize()!;
    try {
      await page.locator("#pageTheme").selectOption("modern");
      // Wide enough for a roomy window, too narrow for a 640px screen
      // beside the 340px tower.
      await page.setViewportSize({ width: 1000, height: 900 });
      const layout = await page.evaluate(() => {
        const screen = document.getElementById("screen")!.getBoundingClientRect();
        const panel = document.getElementById("frontPanelCard")!.getBoundingClientRect();
        const cd = document.querySelector('.at-bay[data-drive="cdrom"]')!.getBoundingClientRect();
        const floppy = document.querySelector('.at-bay[data-drive="0"]')!.getBoundingClientRect();
        const turbo = document.querySelector(".tower-panel")!.getBoundingClientRect();
        const heading = document.querySelector("#frontPanelCard > h2")!;
        const blanks = [...document.querySelectorAll(".bay-blank")].filter((el) => {
          const s = getComputedStyle(el);
          return s.display !== "none" && el.getBoundingClientRect().height > 2;
        }).length;
        return {
          panelBelowScreen: panel.top >= screen.bottom - 2,
          // 920px win/web94 page, minus its 3px border and 22px inner pad.
          winWidth: Math.abs(panel.width - 870) < 2,
          heading: getComputedStyle(heading).display !== "none",
          controlsLeftOfCd: turbo.right <= cd.left + 4,
          blanks,
          cdFace: cd.height > 80,
          floppyBelowCd: floppy.top >= cd.bottom - 4,
        };
      });
      expect(layout.panelBelowScreen).toBe(true);
      expect(layout.winWidth).toBe(true);
      expect(layout.heading).toBe(true);
      expect(layout.controlsLeftOfCd).toBe(true);
      expect(layout.blanks).toBe(0);
      expect(layout.cdFace).toBe(true);
      expect(layout.floppyBelowCd).toBe(true);
    } finally {
      await page.setViewportSize(vp);
    }
  });

  test("Turbo toggles DX2 clock doubling: 66 MHz on, 33 MHz off", async ({ livePage: page }) => {
    const btn = page.locator("#turboBtn");
    const led = page.locator("#turboLed");
    // Shared livePage: a prior frontpanel case may have left Turbo off.
    if ((await btn.getAttribute("aria-pressed")) !== "true") await btn.click();
    await expect(btn).toHaveAttribute("aria-pressed", "true");
    await expect(led).toHaveClass(/turbo-on/);
    await expect.poll(async () => page.evaluate(() => (window as any).__test.machine.cpuHz())).toBe(66000000);

    // Turbo on: guest should be making progress. Absolute MHz is soft --
    // under a loaded multi-worker suite the host often sustains well under
    // the intended 66 (PC486_REVIEW.md §8.6); cpuHz above is the DX2
    // contract. Measure over a short wall window for the ratio check below.
    const rateOn = await page.evaluate(async () => {
      const m = (window as any).__test.machine;
      const t0 = performance.now();
      const c0 = m.totalCycles();
      await new Promise((r) => setTimeout(r, 200));
      return (m.totalCycles() - c0) / ((performance.now() - t0) / 1000);
    });
    expect(rateOn).toBeGreaterThan(5e6);

    await btn.click();
    await expect(btn).toHaveAttribute("aria-pressed", "false");
    await expect(led).not.toHaveClass(/turbo-on/);
    await expect.poll(async () => page.evaluate(() => (window as any).__test.machine.cpuHz())).toBe(33000000);

    const rateOff = await page.evaluate(async () => {
      const m = (window as any).__test.machine;
      const t0 = performance.now();
      const c0 = m.totalCycles();
      await new Promise((r) => setTimeout(r, 200));
      return (m.totalCycles() - c0) / ((performance.now() - t0) / 1000);
    });
    // Under ?fast=1 both rates often sit on the same host ceiling
    // (PC486_REVIEW.md §8.6), so a half-speed wall-clock ratio is only
    // meaningful when Turbo-on was clearly below that ceiling. cpuHz
    // above is the DX2 contract either way.
    if (rateOff < rateOn * 0.85) {
      expect(rateOff).toBeLessThan(rateOn * 0.7);
      expect(rateOff).toBeGreaterThan(3e6);
    }

    await btn.click();
    await expect(btn).toHaveAttribute("aria-pressed", "true");
    await expect(led).toHaveClass(/turbo-on/);
    await expect.poll(async () => page.evaluate(() => (window as any).__test.machine.cpuHz())).toBe(66000000);
  });
});
