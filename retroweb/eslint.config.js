import js from "@eslint/js";
import globals from "globals";

// Browser scripts loaded with <script src>, plus the emscripten factory and
// xterm / Google globals those pages pull in the same way. Generated glue
// (wasm .js, copied web/shared/, vendored xterm) is not source.
export default [
  {
    ignores: [
      "**/node_modules/**",
      "**/vendor/**",
      "_site/**",
      "_site.new/**",
      "**/web/shared/**",
      "**/tests/build/**",
      "**/tests/build-cov/**",
      "**/tests/coverage-cpp/**",
      "**/coverage/**",
      "eslint.config.js",
      "altair8800/web/retro8080.js",
      "assembler6502/web/cgoac6502.js",
      "assembler6502/web/roms/entrypoints.js",
      "ibmpc-at/web/ibmpcat.js",
      "pacman/web/pacman.js",
      "frogger/web/frogger.js",
      "galaxian/web/galaxian.js",
      "galaga/web/galaga.js",
      "scramble/web/scramble.js",
    ],
  },
  {
    files: ["**/*.js"],
    languageOptions: {
      ecmaVersion: 2022,
      sourceType: "script",
      globals: {
        ...globals.browser,
        Terminal: "readonly",
        FitAddon: "readonly",
        Retro8080: "readonly",
        CgOac6502: "readonly",
        CGOAC_ENTRYPOINTS: "readonly",
        PacmanArcade: "readonly",
        FroggerArcade: "readonly",
        GalaxianArcade: "readonly",
        GalagaArcade: "readonly",
        ScrambleArcade: "readonly",
        IbmPcAt: "readonly",
        google: "readonly",
        gapi: "readonly",
        LcdFont: "readonly",
      },
    },
    rules: {
      ...js.configs.recommended.rules,
      // localStorage and a few boot paths swallow failures on purpose.
      "no-empty": ["error", { allowEmptyCatch: true }],
      "no-unused-vars": ["error", {
        argsIgnorePattern: "^_",
        caughtErrors: "none",
        varsIgnorePattern: "^_",
      }],
    },
  },
  {
    files: ["**/app.js"],
    languageOptions: {
      globals: {
        initThemePicker: "readonly",
        initFullscreen: "readonly",
        initFocusHint: "readonly",
      },
    },
  },
  {
    // These files define the shared entry points. Other pages call them.
    files: ["shared/theme-picker.js", "shared/fullscreen.js", "shared/focus-hint.js"],
    languageOptions: {
      globals: {
        ...globals.browser,
      },
    },
    rules: {
      "no-unused-vars": ["error", {
        argsIgnorePattern: "^_",
        caughtErrors: "none",
        varsIgnorePattern: "^(_|initThemePicker|initFullscreen|initFocusHint)$",
      }],
    },
  },
];
