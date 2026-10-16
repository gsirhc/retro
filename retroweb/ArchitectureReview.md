# retroweb Architecture Review

Second pass over all 12 machines, `shared/`, the Makefiles, and the CI
workflows. The first review's items (PC chipset merge, arcade-core, site
staging, fast-flag strip, shared chip tests, CI and Makefile consolidation,
sleep removal, comment style, site size) all landed and are no longer
listed. Findings come from reading and diffing source; no tests or builds
were run for this review.

## Medium priority

### M1. ibmpc-at's C: persistence lags pc486's

`ibmpc-at/web/app.js` (`openHddDb`, `saveHdd`, `persistHddIfDirty`):

- Every 5 s while C: is dirty, the whole 31.9 MB image goes to IndexedDB
  with a `put()` on the main thread. The structured clone runs on the same
  thread that paces the 80286, so a DOS session that writes to disk gets a
  hitch every 5 s. pc486 already solved this: a worker owns storage and
  only the written sectors go across (`hdd-worker.js`, `hddDirtyPatches()`).
- `openHddDb()` opens a new connection on every load, save and clear, and
  never closes one. The arcades fixed the same pattern with H3 (one
  connection, closed on `versionchange`).
- ~~`persistHddIfDirty()` clears the dirty flag before `saveHdd()` has
  committed, so a failed write is never retried.~~ Done: a failed save is
  retried a minute later (`hddRetryAt`), and the label changes only once
  the write commits.

Both PCs, done:

- ~~A failed save only reaches `console.error` and the label says "saved"
  before the write starts.~~ The label now changes only after the write
  commits. A failed read or write shows "Changes to C: won't be there when
  you return" (`HDD_STORAGE_BLOCKED`). pc486 also guards the startup read
  and the blank, seed and upload writes, retries a failed patch save as a
  full image (its dirty sectors are already consumed), and `hdd-worker.js`
  drops a rejected `openDb()` so storage that comes back gets used.
- ~~No "with IndexedDB blocked" case.~~ `tests/hddstorage.spec.ts` in both
  suites: the factory C: boots with storage blocked and warns, and a
  refused save warns, never shows "saved", and is retried once storage
  works. pc486 injects the block into the worker with `page.route`.
- ibmpc-at only: the "Barebones FreeDOS" notice from pc486 shows over the
  factory C: (`tests/bootnotice.spec.ts`).

Remaining: the whole-image main-thread `put()` and the per-call
connections.

Suggested fix: move ibmpc-at onto the same worker and patch path, sharing
`hdd-worker.js` through `SHARED_PC` with the database name and model as
parameters. The pc486 storage handling above then covers ibmpc-at too.

### M2. Twelve `devserve.py` copies in five variants

Every `web/` has its own dev server, and Playwright starts it
(`python3 devserve.py ${PORT}`). The copies have drifted:

- frogger, galaga, galaxian and scramble take `--https`, which nothing passes.
- Only pc486's serves a `.gz` sibling as `Content-Encoding: gzip`, the way
  Pages and `serve_nocache.py` do. The other eleven suites test against a
  server that doesn't match production or `make preview`.
- altair8800 and assembler6502 each have their own one-off copy.

`serve_nocache.py PORT DIR` already does all of it (no-store, gzip siblings,
optional TLS). Point each Playwright config and `make serve` at it
(`python3 ../../serve_nocache.py ${PORT} .`, a dev-only path that never
ships), and delete the copies. Keep its gzip rule as it is: a raw request
gets the `.gz` sibling only when the raw file exists too, because Pages 404s
an undeployed raw file. That rule is what lets pc486's specs catch a page
that names a raw HDD image.

### M3. Arcade Playwright specs are copy-paste

H3 moved the arcade front ends onto `shared/arcade-core.js`, but their
specs stayed eight copies each, about 2,000 lines that test the same
shared code:

| Spec | Copies | Lines | Variants |
|------|--------|-------|----------|
| `theme.spec.ts` | 8 | 880 | 1 |
| `seam.spec.ts` | 8 | 104 | 1 |
| `fullscreen.spec.ts` | 8 | 529 | 3 |
| `focus-hint.spec.ts` | 8 | 208 | 2 |
| `footer.spec.ts` | 8 | 152 | 2 |
| `mute.spec.ts` | 8 | 163 | 2 |

Plus the arcade-core blocks inside each `rom.spec.ts` and `hiscore.spec.ts`
(IndexedDB blocked, stored ROM unreadable, Remove ROMs). The copies have
already drifted: seven cabinets' `fullscreen.spec.ts` check that the canvas
stays centred in the bezel, and asteroids' doesn't. A change to
arcade-core means eight edits, and missing one passes.

Suggested fix, the same move as `arcade-web.yml` for CI: shared specs in
`shared/arcade-tests/`, copied into each cabinet's `web/tests/shared/` by
`make shared` (git-ignored, like `web/shared/`), reading per-cabinet values
(aspect ratio, HIGH SCORE address, POST gate) from a small fixture each
cabinet keeps. Cabinet-specific specs (`keyboard`, `dip`, `coindoor`,
`touch`, `help`, `smoke`) stay local.

## Low priority

- **CLAUDE.md port list.** It lists every Playwright port except pc486's
  `8400`/`8410` and Altair's `8110` (its landing-page server for
  `home.spec.ts`).
- **ibmpc-at typing helpers wait on time, not the guest.** pc486's
  `tap`/`typeStr` wait until the guest has read each scan code
  (`scancodesPending`, 8042 OBF). ibmpc-at's wait a fixed slice of emulated
  time per key, because its wasm exposes no keyboard-controller state. Expose
  it (and a `scancodesPending` getter) and switch the helpers over, ideally
  with the PC front-end item below.
- **POKEY linked low channel.** In 16-bit mode `pokey.cpp` gives both
  channels the 16-bit period, so the low channel sounds at that rate. In MAME
  `pokey.cpp` the low channel keeps borrowing at its own 8-bit rate and only
  clocks the high one. Centipede and Missile don't appear to link channels,
  so nothing audible changes today, but it's a known departure.
- **actionlint isn't run.** All three workflows pass actionlint 1.7.12 and it
  caught a planted input typo, but nothing runs it. Add it to `make lint` (a
  pinned, checksummed binary like the other fetched tools) or a CI step.
- **PC front-end overlap.** About 80 lines are byte-identical between
  `ibmpc-at/web/app.js` and `pc486/web/app.js`: the load overlay
  (`beginLoad`, `endLoad`, `withLoad`, `paintLoadOverlay`), frame sizing
  (`fitScreen`, `setFrameSize`, `presentFrame`, `clearScreenToBlack`),
  `setBayLoaded` and `refreshFkeyControls`. Small enough to leave, but if
  M1 shares the HDD worker, these could go in a `shared/pc-front.js` in the
  same change.

## Suggested order

Keep this list current. When an item lands, strike it through and point to
the entry that records the fix. A new finding goes in its section above and
gets a place here in the same change, so this list is always the whole
backlog.

1. **M1**, ~~the PC storage warning and blocked-storage specs~~ (done, see
   M1), then ibmpc-at onto the worker.
2. **M2**, one dev server. Small, and it makes M3's specs run against the
   same server as production.
3. **M3**, shared arcade specs.
4. **Low**: the CLAUDE.md port list (one line); PC front-end overlap and the
   ibmpc-at typing helpers with M1; actionlint in `make lint`; the POKEY
   linked low channel.
