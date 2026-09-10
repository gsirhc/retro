import { defineConfig, devices } from "@playwright/test";

// Integration suite for the 6502 Assembler emulator front end. Drives the real
// page in a real browser: asserts control behaviour, machine state, and
// xterm buffer text. Modeled on retroweb/altair8800/web/playwright.config.ts.

const PORT = 8200;   // dedicated port so it never collides with a hand-run `make serve`

export default defineConfig({
  testDir: "tests",
  testMatch: /.*\.spec\.ts$/,
  fullyParallel: true,
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 2 : 1,
  // Each page runs a real requestAnimationFrame-paced 1MHz CPU core -- too
  // much parallelism starves individual tabs of real CPU time, which (per
  // machine.h's wall-clock pacing contract) makes the *emulated* boot
  // itself take longer, not just the test. Capped well below the core
  // count rather than left to Playwright's default heuristic.
  workers: process.env.CI ? 2 : 3,
  reporter: process.env.CI ? [["list"], ["html", { open: "never" }]] : [["list"]],

  // Assembling + page-write-timed burns can take real wall-clock seconds
  // (or the "instant burn" toggle skips that -- most specs use it); boot
  // itself gets generous headroom for the same contention reason above.
  timeout: 60_000,
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
