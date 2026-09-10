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
  // itself take longer, not just the test. GitHub Actions' shared 2-vCPU
  // runners can't reliably give two such real-time-CPU-bound Chromium
  // processes a fair scheduling slice -- generous timeouts (see below)
  // brought failures down but didn't eliminate them (CGOAC6502_REVIEW.md
  // #14.c/#14.d), so CI runs this suite serially instead: no contention
  // between workers to begin with, at the cost of longer CI wall-clock
  // time for a suite that's small either way. Local runs keep real
  // parallelism (this repo's own dev hardware tolerates it fine).
  workers: process.env.CI ? 1 : 3,
  reporter: process.env.CI ? [["list"], ["html", { open: "never" }]] : [["list"]],

  // Assembling + page-write-timed burns can take real wall-clock seconds
  // (or the "instant burn" toggle skips that -- most specs use it); boot
  // itself gets generous headroom for the same contention reason above --
  // bumped from 60s/20s after real 2-worker contention (locally and in CI)
  // pushed a from-cold boot past the previous 25s per-assertion margin, an
  // honest (if slow) `expect` timeout that a real xterm.js implementation
  // detail was separately disguising as bogus terminal content (see
  // CGOAC6502_REVIEW.md and the .xterm-rows selector fix in tests/).
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
