import js from "@eslint/js";
import globals from "globals";
import tseslint from "typescript-eslint";

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
        termDumbFilter: "readonly",
        termVt52Filter: "readonly",
        termAdm3aFilter: "readonly",
      },
    },
  },
  {
    files: ["shared/theme-picker.js", "shared/fullscreen.js", "shared/focus-hint.js", "shared/winbtns.js", "shared/pc-keyboard.js", "shared/arcade-touch.js", "shared/arcade-core.js", "shared/disk-drop.js", "shared/disk-library.js", "shared/footer.js", "shared/terminal.js"],
    languageOptions: {
      globals: {
        ...globals.browser,
      },
    },
    rules: {
      "no-unused-vars": ["error", {
        argsIgnorePattern: "^_",
        caughtErrors: "none",
        varsIgnorePattern: "^(_|initThemePicker|initFullscreen|initFocusHint|initDiskDrop|initDiskLibrary|initPcKeyboard|initArcadeTouch|arcadeTouchOnly|arcadeCrc32|arcadeHexCrc|arcadeUnzip|arcadeReadRomFiles|arcadeStore|arcadePrefs|arcadeAudio|arcadeHiscore|arcadeHiscoreReset|arcadeRomLoader|arcadeLoop|addFooterNote|termDumbFilter|termVt52Filter|termAdm3aFilter)$",
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
  ...tseslint.configs.recommended.map((c) => ({ ...c, files: ["**/*.ts"] })),
  {
    files: ["**/*.ts"],
    languageOptions: { globals: { ...globals.node, ...globals.browser } },
    rules: {
      "@typescript-eslint/no-explicit-any": "off",
      "@typescript-eslint/no-non-null-assertion": "off",
      "no-restricted-syntax": ["error",
        {
          selector: "CallExpression[callee.property.name='waitForTimeout']",
          message: "Wait on a condition (expect, expect.poll, waitForFunction), not a fixed time.",
        },
        {
          selector: "NewExpression[callee.name='Promise'] CallExpression[callee.name='setTimeout']",
          message: "A setTimeout sleep is waitForTimeout by another name. Wait on a condition.",
        },
      ],
    },
  },
];
