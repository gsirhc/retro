import { test, expect } from "@playwright/test";

test("the test seam is absent without ?test", async ({ page }) => {
  await page.goto("/");
  // __test is set right after the frame loop starts, so a painted frame means it would exist by now.
  await page.waitForFunction(() => {
    const c = document.getElementById("screen") as HTMLCanvasElement;
    const d = c.getContext("2d")!.getImageData(0, 0, c.width, c.height).data;
    for (let i = 0; i < d.length; i += 4) if (d[i] || d[i + 1] || d[i + 2]) return true;
    return false;
  });
  expect(await page.evaluate(() => (window as any).__test)).toBeUndefined();
});
