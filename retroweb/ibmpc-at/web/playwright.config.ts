import { defineConfig, devices } from "@playwright/test";

// Playwright suite for the IBM PC/AT front end. textScreen() is test-only (wasm_machine.cpp).

const PORT = 8300;      // the emulator front end (this dir)
const HOME_PORT = 8310; // the retroweb/ landing page, for home.spec.ts

export default defineConfig({
  testDir: "tests",
  testMatch: /.*\.spec\.ts$/,
  fullyParallel: true,
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 2 : 0,
  failOnFlakyTests: !!process.env.FAIL_ON_FLAKY,
  // One worker: each page runs a real-time 8 MHz core, and parallel boots starve each other.
  // Coverage also aggregates per worker.
  workers: process.env.COVERAGE ? 1 : process.env.CI ? 2 : 3,
  globalSetup: process.env.COVERAGE ? "./tests/coverage.setup.ts" : undefined,
  globalTeardown: process.env.COVERAGE ? "./tests/coverage.global.ts" : undefined,
  reporter: process.env.CI ? [["list"], ["github"], ["html", { open: "never" }]] : [["list"]],

  // POST + FreeDOS boot takes tens of real seconds, more under contention.
  timeout: 120_000,
  expect: { timeout: 20_000 },

  use: {
    baseURL: `http://localhost:${PORT}`,
    trace: "on-first-retry",
    screenshot: "only-on-failure",
    video: "off",
  },

  projects: [{ name: "chromium", use: { ...devices["Desktop Chrome"] } }],

  webServer: [
    {
      command: `python3 ../../serve_nocache.py ${PORT} .`,
      url: `http://localhost:${PORT}/`,
      reuseExistingServer: !process.env.CI,
      timeout: 20_000,
      stdout: "ignore",
      stderr: "pipe",
    },
    {
      // retroweb/ for the landing-page spec
      command: `python3 ../../serve_nocache.py ${HOME_PORT} ../..`,
      url: `http://localhost:${HOME_PORT}/`,
      reuseExistingServer: !process.env.CI,
      timeout: 20_000,
      stdout: "ignore",
      stderr: "pipe",
    },
  ],
});
