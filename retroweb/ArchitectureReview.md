# retroweb Architecture Review

The open backlog from three passes over all 12 machines, `shared/`, the
Makefiles, `watch_site.py` and the CI workflows. Finished items are
removed; their history is in git and each machine's review doc. Nothing
here is broken today. These are cleanups.

## Medium priority

### M5. Thirteen npm projects

Each machine's `web/` has its own `package.json`, lockfile and ~18-22 MB
`node_modules`, plus `retroweb/package.json` for lint. All thirteen pin
`@playwright/test` to `1.63.0`, so versions agree, but a bump means
thirteen `npm install`s and CI runs `npm ci` per job.

Suggested fix: make `retroweb/` an npm workspace root (`"workspaces":
["*/web"]`), one lockfile. `make test-install` and the CI `npm ci` steps
then run once from `retroweb/`.

### M7. The machine list lives in too many places

Adding a machine means touching about a dozen lists (CLAUDE.md's wiring
checklist). Two of them repeat whole build recipes rather than names:

- `retroweb/Makefile` `_stage` and `deploy-emulator.yml`'s `build` job
  each spell out every machine's targets (`ibmpcat.js roms hdd-image
  library-images preset-images shared`, ...). They match today but are
  maintained separately.
- `make preview`'s URL echo is a third copy of `MACHINES`.

The `build` job also recompiles every wasm module that the `*-web-test`
jobs just built and tested, so the deployed binary is a second build of
the tested one.

Suggested fix: a `site-deps` target in `shared/web.mk` (default `wasm
roms shared`, overridden in the PC and terminal Makefiles). `_stage`
becomes a loop over `MACHINES`, CI's `build` job calls `make -C retroweb
_stage` (keeping its cache steps for the slow PC images in front), and the
URL echo loops too. Shipping the tested artifacts is optional on top.

## Low priority

- **PC front-end overlap.** About 100 lines are near-identical between
  `ibmpc-at/web/app.js` and `pc486/web/app.js`: the load overlay
  (`beginLoad`, `endLoad`, `withLoad`, `paintLoadOverlay`), frame sizing
  (`fitScreen`, `setFrameSize`, `presentFrame`, `clearScreenToBlack`),
  `setBayLoaded`, `refreshFkeyControls`, and the `hdd-worker.js` client
  and save queue. These could go in a `shared/pc-front.js`.
- **Forked PC chips.** `wd1003`, `i8042` and `ega` stay separate copies
  (IDE vs MFM, the PS/2 aux port and VGA are real differences). When a
  protocol fix lands in one, check the other's copy in the same change.
- **ibmpc-at reaches into pc486.** `ibmpc-at/Makefile` and
  `disks/build-preset-hdd.sh` use `../pc486/disks/zero-free-clusters.py`
  and pc486's FreeDOS CD (`$(MAKE) -C ../pc486 disks/freedos-cd.iso`).
  Move the script to `shared/dos-library/` and the CD fetch beside it, so
  neither PC builds out of the other's folder.
- **CI cache keys repeated.** The PC image cache keys (`ibmpcat-hdd`,
  `pc486-hdd`, `pc486-cd`, both `*-preset-hdds`) are spelled out about 20
  times across restore/save pairs in both workflows. They agree today. A
  composite action per image, like `.github/actions/fetch-altair-media`,
  would define each key once.
- **Theme bootstrap sync.** `shared/theme-init.js` and its three inlined
  copies (`index.html`, `about.html`, `arcade/index.html`) match today,
  but nothing checks it. A `lint-staging` step that minifies the shared
  file and compares it to each inline `<script>` would hold them together.

## Not planned

- **Arcade page scaffolding.** Each cabinet's `app.js` repeats keys, coin
  door, DIPs and `window.__test` around `arcade-core.js`, and each
  `index.html` inlines ~110 lines of CSS. The per-cabinet code is where the
  boards really differ, and the shared specs in `shared/arcade-tests/`
  already guard the common surface.

## Next review

Bugs and fidelity only, not refactors or duplication.
