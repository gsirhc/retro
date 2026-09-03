import { defineConfig, devices } from "@playwright/test";

// Integration + fidelity suite for the Altair 8800 emulator front end.
// Drives the real page in a real browser: asserts control behaviour, machine
// state, and xterm buffer text. No screenshot/visual comparison.
//
// The page is served by devserve.py (no-cache headers) on a dedicated port so
// it never collides with a hand-run `make serve` on 8000.

const PORT = 8100;

export default defineConfig({
  testDir: "tests",
  testMatch: /.*\.spec\.ts$/,
  fullyParallel: true,
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 2 : 0,
  // coverage aggregates per-worker; one worker keeps it simple and exact
  workers: process.env.COVERAGE ? 1 : process.env.CI ? 2 : undefined,
  globalSetup: process.env.COVERAGE ? "./tests/coverage.setup.ts" : undefined,
  globalTeardown: process.env.COVERAGE ? "./tests/coverage.global.ts" : undefined,
  reporter: process.env.CI ? [["list"], ["html", { open: "never" }]] : [["list"]],

  // Game listing injection runs the CPU at a real 2 MHz (plus the sanctioned
  // load-time `turbo`); a few specs need ~20 s end to end.
  timeout: 60_000,
  expect: { timeout: 12_000 },

  use: {
    baseURL: `http://localhost:${PORT}`,
    trace: "on-first-retry",
    screenshot: "only-on-failure",
    video: "off",
  },

  projects: [{ name: "chromium", use: { ...devices["Desktop Chrome"] } }],

  webServer: {
    command: `python3 devserve.py ${PORT}`,
    url: `http://localhost:${PORT}/`,
    reuseExistingServer: !process.env.CI,
    timeout: 20_000,
    stdout: "ignore",
    stderr: "pipe",
  },
});
