import { defineConfig, devices } from "@playwright/test";

// Integration suite for the pc486 front end, driving the real page. State comes from the
// `?test=1`-gated window.__test seam in app.js and Machine::textScreen().

const PORT = 8400;
const HOME_PORT = 8410;

export default defineConfig({
  testDir: "tests",
  testMatch: /.*\.spec\.ts$/,
  fullyParallel: true,
  forbidOnly: !!process.env.CI,
  // One retry clears a flake; more turns a stuck boot into a multi-minute stall.
  retries: process.env.CI ? 1 : 0,
  // The guest is host-bound near 66 MHz even with fast=1 (PC486_REVIEW.md §8.6). Each worker mounts
  // the 256MB HDD once, so extra workers split the CPU. Default 1 on CI, 2 locally; PW_WORKERS=N overrides.
  workers: process.env.COVERAGE
    ? 1
    : (process.env.PW_WORKERS
      ? Math.max(1, Number(process.env.PW_WORKERS) || 1)
      : (process.env.CI ? 1 : 2)),

  globalSetup: process.env.COVERAGE ? "./tests/coverage.setup.ts" : undefined,
  globalTeardown: process.env.COVERAGE ? "./tests/coverage.global.ts" : undefined,
  reporter: process.env.CI ? [["list"], ["html", { open: "never" }]] : [["list"]],

  // FreeDOS to C:\> is host-bound wall clock; tests that don't need the prompt use bootLive().
  timeout: 90_000,
  expect: { timeout: 15_000 },

  use: {
    baseURL: `http://localhost:${PORT}`,
    trace: "on-first-retry",
    screenshot: "only-on-failure",
    video: "off",
  },

  projects: [{ name: "chromium", use: { ...devices["Desktop Chrome"] } }],

  webServer: [
    {
      command: `python3 devserve.py ${PORT}`,
      url: `http://localhost:${PORT}/`,
      reuseExistingServer: !process.env.CI,
      timeout: 20_000,
      stdout: "ignore",
      stderr: "pipe",
    },
    {
      // retroweb/ is two levels up, for home.spec.ts.
      command: `python3 -m http.server ${HOME_PORT} -d ../..`,
      url: `http://localhost:${HOME_PORT}/`,
      reuseExistingServer: !process.env.CI,
      timeout: 20_000,
      stdout: "ignore",
      stderr: "pipe",
    },
  ],
});
