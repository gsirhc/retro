import { test as base, expect } from "@playwright/test";
import { CoverageReport } from "monocart-coverage-reports";
import { coverageEnabled, coverageOptions } from "./coverage";

// Specs import { test, expect } from here so V8 coverage for app.js is collected under COVERAGE=1.

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
