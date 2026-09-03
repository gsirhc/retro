// Shared monocart-coverage-reports config. Coverage is opt-in: it only runs
// when COVERAGE=1 is set (see `make coverage`). We only care about app.js --
// the hand-written front-end logic -- not xterm, the wasm glue, or the specs.

export const coverageEnabled = !!process.env.COVERAGE;

export const coverageOptions = {
  name: "retro8080 app.js coverage",
  outputDir: "./coverage",
  reports: [
    ["v8", { metrics: ["lines", "functions", "branches"] }],
    ["console-details"],
    ["lcovonly"],
  ],
  // keep only our own source; drop everything else V8 reports
  entryFilter: {
    "**/app.js*": true,
    "**/*": false,
  },
  sourceFilter: {
    "**/app.js*": true,
  },
  // treat query-string builds (app.js?v=123) as the same file
  onEntry: async (entry: any) => {
    entry.url = entry.url.replace(/\?.*$/, "");
  },
};
