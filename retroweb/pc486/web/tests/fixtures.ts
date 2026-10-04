import { test as base, expect, Page } from "@playwright/test";
import { CoverageReport } from "monocart-coverage-reports";
import { coverageEnabled, coverageOptions } from "./coverage";
import { boot, bootLive, resetLivePage, resetPromptPage } from "./helpers";

// Every spec imports { test, expect } from here instead of @playwright/test so
// that V8 coverage for app.js is collected automatically when COVERAGE=1.
//
// Worker-scoped pages (livePage / perfPage / promptPage) mount the 256MB HDD
// once per worker and reset between tests. Prefer those over boot()/bootLive()
// whenever the test does not need a fresh navigation (first-visit localStorage,
// reload persistence, route.abort, addInitScript, realtime smoke, etc.).

type WorkerFixtures = {
  _sharedLivePage: Page;
  _sharedPerfPage: Page;
  _sharedPromptPage: Page;
};

type TestFixtures = {
  livePage: Page;
  perfPage: Page;
  promptPage: Page;
  _coverage: void;
};

export const test = base.extend<TestFixtures, WorkerFixtures>({
  _sharedLivePage: [
    async ({ browser }, use) => {
      const page = await browser.newPage();
      await bootLive(page);
      await use(page);
      await page.close();
    },
    { scope: "worker" },
  ],

  _sharedPerfPage: [
    async ({ browser }, use) => {
      const page = await browser.newPage();
      await bootLive(page, { params: "perf=1" });
      await use(page);
      await page.close();
    },
    { scope: "worker" },
  ],

  _sharedPromptPage: [
    async ({ browser }, use) => {
      const page = await browser.newPage();
      await boot(page);
      await use(page);
      await page.close();
    },
    { scope: "worker" },
  ],

  livePage: async ({ _sharedLivePage }, use) => {
    await resetLivePage(_sharedLivePage);
    await use(_sharedLivePage);
  },

  perfPage: async ({ _sharedPerfPage }, use) => {
    await resetLivePage(_sharedPerfPage);
    await use(_sharedPerfPage);
  },

  promptPage: async ({ _sharedPromptPage }, use) => {
    await resetPromptPage(_sharedPromptPage);
    await use(_sharedPromptPage);
  },

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
