// monocart-coverage-reports config. Opt-in via COVERAGE=1, app.js only.

export const coverageEnabled = !!process.env.COVERAGE;

export const coverageOptions = {
  name: "ibmpc-at app.js coverage",
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
