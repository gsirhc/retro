import { defineConfig, devices } from "@playwright/test";

// Integration suite for the Pac-Man arcade emulator front end. Drives the
// real page in a real browser: asserts control behaviour, machine state
// (via the `?test=1`-gated window.__test seam app.js exposes), and canvas
// pixel contents. Modeled on retroweb/assembler6502/web/playwright.config.ts.

const PORT = 8500;      // the emulator front end (this dir)
const HOME_PORT = 8510; // the retroweb/ landing page, for home.spec.ts

export default defineConfig({
  testDir: "tests",
  testMatch: /.*\.spec\.ts$/,
  fullyParallel: true,
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 2 : 0,
  // A real requestAnimationFrame-paced 3.072 MHz Z80 core per page -- same
  // "don't starve a real-time-paced tab of wall-clock CPU" reasoning as the
  // other machines' configs, though this board is by far the cheapest to
  // emulate of any machine in this repo, so parallelism is generous.
  workers: process.env.CI ? 2 : 4,
  reporter: process.env.CI ? [["list"], ["html", { open: "never" }]] : [["list"]],

  timeout: 30_000,
  expect: { timeout: 10_000 },

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
