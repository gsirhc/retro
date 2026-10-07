import { defineConfig, devices } from "@playwright/test";

const PORT = 8200;

export default defineConfig({
  testDir: "tests",
  testMatch: /.*\.spec\.ts$/,
  fullyParallel: true,
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 2 : 1,
  // The 1MHz core is wall-clock paced, so CPU contention slows the emulated
  // boot. CI's 2-vCPU runners flake with parallel workers (CGOAC6502_REVIEW.md
  // #14.c/#14.d), so CI runs serially.
  workers: process.env.CI ? 1 : 3,
  reporter: process.env.CI ? [["list"], ["html", { open: "never" }]] : [["list"]],

  // Assembling and timed burns take real seconds; boot needs headroom under contention.
  timeout: 90_000,
  expect: { timeout: 20_000 },

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
