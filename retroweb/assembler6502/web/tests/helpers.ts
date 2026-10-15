import { Page } from "@playwright/test";

/** Wait until the page has run `ms` of emulated 1 MHz time. */
export async function runFor(page: Page, ms: number): Promise<void> {
  const target = (await page.evaluate(() => (window as any).__machine.cycleCount())) + ms * 1000;
  await page.waitForFunction((t) => (window as any).__machine.cycleCount() >= t, target);
}

/** Wait until the ROM has consumed every typed byte, then `ms` more of emulated time. */
export async function settle(page: Page, ms: number): Promise<void> {
  await page.waitForFunction(() => (window as any).__inputPending() === 0);
  await runFor(page, ms);
}

/** Wait for `n` animation frames in the page. */
export function frames(page: Page, n: number): Promise<void> {
  return page.evaluate(
    (n) =>
      new Promise<void>((resolve) => {
        const tick = () => (n-- > 0 ? requestAnimationFrame(tick) : resolve());
        tick();
      }),
    n,
  );
}
