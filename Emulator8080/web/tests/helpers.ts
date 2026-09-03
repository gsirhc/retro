import { Page, expect } from "@playwright/test";

// Shared helpers for the emulator integration suite. Everything drives the real
// page; `window.__test` (present only under `?test=1`) is the inspection seam.

export const TEST_QS = "test=1";

/** Navigate to the emulator with `?test=1` and wait for the machine to be live. */
export async function boot(
  page: Page,
  opts: { params?: string; expectText?: RegExp } = {},
): Promise<void> {
  const qs = opts.params ? `${TEST_QS}&${opts.params}` : TEST_QS;
  await page.goto(`/?${qs}`);
  await page.waitForFunction(() => !!(window as any).__test?.machine, null, {
    timeout: 15_000,
  });
  // the built-in echo ROM (or a preset) is running once state() responds
  await page.waitForFunction(() => {
    try {
      return typeof (window as any).__test.regs().pc === "number";
    } catch {
      return false;
    }
  });
  // a preset applies ~200 ms after load; applyPreset writes retro8080.preset to
  // localStorage only after it has set RAM / terminal / backplane / devices, so
  // that's the "hardware is configured" signal.
  const wantPreset = /(?:^|&)preset=([a-z]+)/.exec(opts.params || "")?.[1];
  if (wantPreset !== undefined) {
    await page.waitForFunction(
      (id) => {
        try {
          return localStorage.getItem("retro8080.preset") === id;
        } catch {
          return false;
        }
      },
      wantPreset,
      { timeout: 10_000 },
    );
    // let applyPreset finish threading/inserting the preset's media
    await page.waitForFunction(() => (window as any).__test.applyingPreset === false, null, {
      timeout: 10_000,
    });
  }
  if (opts.expectText) await waitForScreen(page, opts.expectText);
}

/** Current xterm buffer contents as plain text. */
export function screen(page: Page): Promise<string> {
  return page.evaluate(() => (window as any).__test.screen());
}

/** Poll the terminal buffer until it matches (or time out). */
export async function waitForScreen(
  page: Page,
  re: RegExp,
  timeout = 30_000,
): Promise<void> {
  await expect
    .poll(() => screen(page), { timeout, message: `screen never matched ${re}` })
    .toMatch(re);
}

/** Feed bytes straight into the 2SIO RX FIFO (as if typed on the console). */
export async function send(page: Page, str: string): Promise<void> {
  await page.evaluate((s) => {
    const m = (window as any).__test.machine;
    for (let i = 0; i < s.length; i++) m.sendByte(s.charCodeAt(i));
  }, str);
}

/** Type on the real keyboard into the focused xterm (tests the kbd->machine wiring). */
export async function pressKeys(page: Page, str: string): Promise<void> {
  await page.evaluate(() => (window as any).__test.term.focus());
  await page.keyboard.type(str, { delay: 12 });
}

/** Machine register snapshot. */
export function regs(page: Page): Promise<any> {
  return page.evaluate(() => (window as any).__test.regs());
}

export function memAt(page: Page, addr: number): Promise<number> {
  return page.evaluate((a) => (window as any).__test.machine.readByte(a & 0xffff), addr);
}

/** Set the 16 address/data toggle switches to `word` by clicking the cells. */
export async function setSwitches(page: Page, word: number): Promise<void> {
  await page.evaluate((w) => {
    const cells = [
      ...document.querySelectorAll<HTMLElement>("#altair .fp-swgrid .fp-cell.paddle"),
    ];
    for (const c of cells) {
      const m = (c.title || "").match(/^A(\d+)/);
      if (!m) continue;
      const bit = +m[1];
      const want = (w >> bit) & 1;
      const bat = c.querySelector(".bat")!;
      const isOn = !bat.classList.contains("down");
      if (isOn !== !!want) (c as HTMLElement).click();
    }
  }, word >>> 0);
}

/**
 * Flip a control paddle. `half` picks the upper (red) or lower (grey) function
 * on a two-way paddle; the latching STOP/RUN paddle ignores it.
 */
export async function clickPaddle(
  page: Page,
  label: RegExp | string,
  half: "up" | "down" = "up",
): Promise<void> {
  const src = typeof label === "string" ? label : label.source;
  const flags = typeof label === "string" ? "i" : label.flags || "i";
  await page.evaluate(
    ({ src, flags, half }) => {
      const want = new RegExp(src, flags);
      const cells = [
        ...document.querySelectorAll<HTMLElement>("#altair .fp-ctl .fp-cell.paddle"),
      ];
      const cell = cells.find((c) => want.test(c.textContent || ""));
      if (!cell) throw new Error(`no control paddle matching ${want}`);
      const bat = (cell.querySelector(".bat") as HTMLElement) || cell;
      const r = bat.getBoundingClientRect();
      const y = half === "up" ? r.top + 2 : r.bottom - 2;
      cell.dispatchEvent(
        new MouseEvent("click", {
          bubbles: true,
          cancelable: true,
          clientX: r.left + r.width / 2,
          clientY: y,
        }),
      );
    },
    { src, flags, half },
  );
}

/** STOP the CPU via the front-panel latching paddle. */
export async function panelStop(page: Page): Promise<void> {
  await page.evaluate(() => {
    if ((window as any).__test.running) {
      const run = [
        ...document.querySelectorAll<HTMLElement>("#altair .fp-ctl .fp-cell.paddle"),
      ].find((c) => /STOP/.test(c.textContent || "") && /RUN/.test(c.textContent || ""));
      run?.click();
    }
  });
}

/** RUN the CPU via the front-panel latching paddle. */
export async function panelRun(page: Page): Promise<void> {
  await page.evaluate(() => {
    if (!(window as any).__test.running) {
      const run = [
        ...document.querySelectorAll<HTMLElement>("#altair .fp-ctl .fp-cell.paddle"),
      ].find((c) => /STOP/.test(c.textContent || "") && /RUN/.test(c.textContent || ""));
      run?.click();
    }
  });
}

/** Is a catalog `<option>` present and selectable (media file actually fetched)? */
export async function hasCatalogItem(
  page: Page,
  selectSel: string,
  label: RegExp,
): Promise<boolean> {
  // wait out the "loading catalog..." placeholder
  await page
    .waitForFunction(
      (selectSel) => {
        const sel = document.querySelector<HTMLSelectElement>(selectSel);
        return !!sel && ![...sel.options].some((o) => /loading catalog/i.test(o.textContent || ""));
      },
      selectSel,
      { timeout: 15_000 },
    )
    .catch(() => {});
  return page.evaluate(
    ({ selectSel, src, flags }) => {
      const re = new RegExp(src, flags);
      const sel = document.querySelector<HTMLSelectElement>(selectSel);
      if (!sel) return false;
      return [...sel.options].some((o) => re.test(o.textContent || "") && !o.disabled);
    },
    { selectSel, src: label.source, flags: label.flags },
  );
}

/** Wait until a catalog `<select>` has an `<option>` whose text matches. */
export async function waitForCatalogItem(
  page: Page,
  selectSel: string,
  label: RegExp,
): Promise<void> {
  await page.waitForFunction(
    ({ selectSel, src, flags }) => {
      const re = new RegExp(src, flags);
      const sel = document.querySelector<HTMLSelectElement>(selectSel);
      return !!sel && [...sel.options].some((o) => re.test(o.textContent || ""));
    },
    { selectSel, src: label.source, flags: label.flags },
    { timeout: 15_000 },
  );
}

/** Pick a catalog `<option>` by visible-text regex and fire `change`. */
export async function pickCatalogItem(
  page: Page,
  selectSel: string,
  label: RegExp,
): Promise<void> {
  await waitForCatalogItem(page, selectSel, label);
  await page.evaluate(
    ({ selectSel, src, flags }) => {
      const re = new RegExp(src, flags);
      const sel = document.querySelector<HTMLSelectElement>(selectSel)!;
      const i = [...sel.options].findIndex((o) => re.test(o.textContent || ""));
      if (i < 0) throw new Error(`no option matching ${re} in ${selectSel}`);
      sel.value = String(sel.options[i].value);
      sel.dispatchEvent(new Event("change"));
    },
    { selectSel, src: label.source, flags: label.flags },
  );
}

/** Replace window.showSaveFilePicker with a spy; recovers bytes into __savedBytes. */
export async function stubSavePicker(page: Page): Promise<void> {
  await page.addInitScript(() => {
    (window as any).__savedBytes = null;
    (window as any).showSaveFilePicker = async () => ({
      createWritable: async () => ({
        write: async (data: any) => {
          const buf =
            data instanceof Blob ? new Uint8Array(await data.arrayBuffer()) : new Uint8Array(data);
          (window as any).__savedBytes = Array.from(buf);
        },
        close: async () => {},
      }),
    });
  });
}
