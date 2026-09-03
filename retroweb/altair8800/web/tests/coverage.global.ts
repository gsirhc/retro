import { CoverageReport } from "monocart-coverage-reports";
import { coverageEnabled, coverageOptions } from "./coverage";

// Playwright calls the default export of the globalSetup / globalTeardown files.
// One module, two entry points -- pick with the COVERAGE_PHASE env the config sets.

export async function coverageSetup() {
  if (!coverageEnabled) return;
  new CoverageReport(coverageOptions).cleanCache();
}

export async function coverageTeardown() {
  if (!coverageEnabled) return;
  const report: any = await new CoverageReport(coverageOptions).generate();
  const s = report?.summary;
  if (s) {
    const pct = (m: any) => (m && typeof m.pct === "number" ? `${m.pct.toFixed(1)}%` : "n/a");
    // eslint-disable-next-line no-console
    console.log(
      `\napp.js coverage — lines ${pct(s.lines)}  functions ${pct(s.functions)}  branches ${pct(s.branches)}` +
        `\n  full report: retroweb/altair8800/web/coverage/index.html\n`,
    );
  }
}

export default coverageTeardown;
