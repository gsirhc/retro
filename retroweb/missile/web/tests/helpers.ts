import type { Page } from "@playwright/test";

export const frames = (page: Page, n: number) =>
  page.evaluate((n) => new Promise<void>((done) => {
    const tick = () => (--n > 0 ? requestAnimationFrame(tick) : done());
    requestAnimationFrame(tick);
  }), n);
