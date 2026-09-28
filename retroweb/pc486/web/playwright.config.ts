import { defineConfig, devices } from "@playwright/test";

// Integration suite for the 486 DX2-66 Gaming PC emulator front end. Drives
// the real page in a real browser: asserts control behaviour, machine
// state (via the `?test=1`-gated window.__test seam app.js exposes), and
// the VGA text-mode screen (via Machine::textScreen(), a test-only
// convenience -- see wasm_machine.cpp). Modeled on
// retroweb/ibmpc-at/web/playwright.config.ts.

const PORT = 8400;      // the emulator front end (this dir)
const HOME_PORT = 8410; // the retroweb/ landing page, for home.spec.ts

export default defineConfig({
  testDir: "tests",
  testMatch: /.*\.spec\.ts$/,
  fullyParallel: true,
  forbidOnly: !!process.env.CI,
  // One retry is enough to clear a flake; a second retry turns a stuck boot
  // into a multi-minute stall (each attempt can wait the full test timeout).
  retries: process.env.CI ? 1 : 0,
  // The guest is host-bound near real 66 MHz even under `fast=1`
  // (PC486_REVIEW.md §8.6). Worker-scoped livePage/perfPage/promptPage
  // fixtures mount the 504MB HDD once per worker; extra workers multiply
  // those mounts and split the CPU ceiling, so CI pins to one. Coverage
  // aggregates per-worker, so it also pins to one.
  workers: process.env.COVERAGE ? 1 : process.env.CI ? 1 : 2,
  globalSetup: process.env.COVERAGE ? "./tests/coverage.setup.ts" : undefined,
  globalTeardown: process.env.COVERAGE ? "./tests/coverage.global.ts" : undefined,
  reporter: process.env.CI ? [["list"], ["html", { open: "never" }]] : [["list"]],

  // FreeDOS to C:\> is host-bound wall clock (tens of seconds). Headroom for
  // CI scheduling jitter; tests that don't need the prompt use bootLive().
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
      // retroweb/ (two levels up) for the landing-page spec
      command: `python3 -m http.server ${HOME_PORT} -d ../..`,
      url: `http://localhost:${HOME_PORT}/`,
      reuseExistingServer: !process.env.CI,
      timeout: 20_000,
      stdout: "ignore",
      stderr: "pipe",
    },
  ],
});
