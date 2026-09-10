import { defineConfig, devices } from "@playwright/test";

// Integration suite for the IBM PC/AT emulator front end. Drives the real
// page in a real browser: asserts control behaviour, machine state (via the
// `?test=1`-gated window.__test seam app.js exposes), and the EGA text-mode
// screen (via Machine::textScreen(), a test-only convenience -- see
// wasm_machine.cpp -- since this machine has no serial-terminal buffer the
// way altair8800/assembler6502 do). Modeled on
// retroweb/altair8800/web/playwright.config.ts.

const PORT = 8300;      // the emulator front end (this dir)
const HOME_PORT = 8310; // the retroweb/ landing page, for home.spec.ts

export default defineConfig({
  testDir: "tests",
  testMatch: /.*\.spec\.ts$/,
  fullyParallel: true,
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 2 : 0,
  // Each page runs a real requestAnimationFrame-paced 8 MHz 80286 core, and
  // POST + a FreeDOS boot is genuinely tens of real seconds (see CLAUDE.md's
  // "Never speed these up" -- the CPU clock is never sped up, not even under
  // test) -- too much parallelism just makes every tab's boot take longer by
  // starving it of real wall-clock CPU time, not faster overall. Coverage
  // aggregates per-worker, so it also pins to one worker for the same reason
  // altair8800's config does.
  workers: process.env.COVERAGE ? 1 : process.env.CI ? 2 : 3,
  globalSetup: process.env.COVERAGE ? "./tests/coverage.setup.ts" : undefined,
  globalTeardown: process.env.COVERAGE ? "./tests/coverage.global.ts" : undefined,
  reporter: process.env.CI ? [["list"], ["html", { open: "never" }]] : [["list"]],

  // A real POST + FreeDOS boot at genuine 8 MHz is tens of real seconds, and
  // slower still under CI/system contention -- generous headroom, same
  // reasoning as altair8800's own real-2MHz-boot timeout.
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
