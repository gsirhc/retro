import js from "@eslint/js";
import globals from "globals";

// Browser scripts, emscripten factories and xterm globals. Generated glue
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
      "asteroids/web/asteroids.js",
      "centipede/web/centipede.js",
      "missile/web/missile.js",
      "pc486/web/pc486.js",
      "pc486/web/pc486-perf.js",
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
        AsteroidsArcade: "readonly",
        CentipedeArcade: "readonly",
        MissileArcade: "readonly",
        IbmPcAt: "readonly",
        Pc486: "readonly",
        LcdFont: "readonly",
      },
    },
    rules: {
      ...js.configs.recommended.rules,
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
        addFooterNote: "readonly",
        initDiskDrop: "readonly",
        initDiskLibrary: "readonly",
        initPcKeyboard: "readonly",
        initArcadeTouch: "readonly",
        arcadeCrc32: "readonly",
        arcadeHexCrc: "readonly",
        arcadeUnzip: "readonly",
        arcadeReadRomFiles: "readonly",
        arcadeStore: "readonly",
        arcadePrefs: "readonly",
        arcadeAudio: "readonly",
        arcadeHiscore: "readonly",
        arcadeHiscoreReset: "readonly",
        arcadeRomLoader: "readonly",
        arcadeLoop: "readonly",
      },
    },
  },
  {
    files: ["shared/theme-picker.js", "shared/fullscreen.js", "shared/focus-hint.js", "shared/winbtns.js", "shared/pc-keyboard.js", "shared/arcade-touch.js", "shared/arcade-core.js", "shared/disk-drop.js", "shared/disk-library.js", "shared/footer.js"],
    languageOptions: {
      globals: {
        ...globals.browser,
      },
    },
    rules: {
      "no-unused-vars": ["error", {
        argsIgnorePattern: "^_",
        caughtErrors: "none",
        varsIgnorePattern: "^(_|initThemePicker|initFullscreen|initFocusHint|initDiskDrop|initDiskLibrary|initPcKeyboard|initArcadeTouch|arcadeTouchOnly|arcadeCrc32|arcadeHexCrc|arcadeUnzip|arcadeReadRomFiles|arcadeStore|arcadePrefs|arcadeAudio|arcadeHiscore|arcadeHiscoreReset|arcadeRomLoader|arcadeLoop|addFooterNote)$",
      }],
    },
  },
  {
    // pc486's IndexedDB worker, which loads the converter with importScripts.
    files: ["pc486/web/hdd-worker.js"],
    languageOptions: {
      globals: {
        ...globals.worker,
        convertLegacyHdd: "readonly",
      },
    },
  },
  {
    files: ["pc486/web/hdd-convert.js"],
    languageOptions: {
      globals: {
        ...globals.worker,
      },
    },
    rules: {
      "no-unused-vars": ["error", {
        argsIgnorePattern: "^_",
        caughtErrors: "none",
        varsIgnorePattern: "^(_|convertLegacyHdd)$",
      }],
    },
  },
];
