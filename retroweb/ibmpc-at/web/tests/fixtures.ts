import { test as base, expect } from "@playwright/test";
import { CoverageReport } from "monocart-coverage-reports";
import { coverageEnabled, coverageOptions } from "./coverage";

// Every spec imports { test, expect } from here instead of @playwright/test so
// that V8 coverage for app.js is collected automatically when COVERAGE=1.

export const test = base.extend<{ _coverage: void }>({
  _coverage: [
    async ({ page, browserName }, use) => {
      const on = coverageEnabled && browserName === "chromium";
      if (on) {
        await page.coverage.startJSCoverage({ resetOnNavigation: false });
      }
      await use();
      if (on) {
        const entries = await page.coverage.stopJSCoverage();
        const mcr = new CoverageReport(coverageOptions);
        await mcr.add(entries);
      }
    },
    { auto: true },
  ],
});

export { expect };
