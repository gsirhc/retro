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

### ~~M2. Twelve `devserve.py` copies in five variants~~ Done

Every Playwright config (both its servers) and every `make serve` run
`python3 ../../serve_nocache.py PORT DIR`, and the twelve `devserve.py`
copies are gone. Every suite now gets no-store and the gzip-sibling rule
(raw file must exist) the same way `make preview` and Pages do.

### ~~M3. Arcade Playwright specs are copy-paste~~ Done

`theme`, `seam`, `fullscreen`, `focus-hint`, `footer` and `mute` live once
in `shared/arcade-tests/`, plus `storage.spec.ts`: the arcade-core blocks
from each `rom.spec.ts` (stored set unreadable, the open store giving way
to `deleteDatabase`, IndexedDB blocked). `arcade.mk` sets `SHARED_TESTS`
and `make shared` copies them into each cabinet's git-ignored
`web/tests/shared/`. Each cabinet's `web/tests/cabinet.ts` gives the ROM
database name, the hwtest files (`rom.spec.ts` imports them too) and the
byte flipped to make a user set.

The drift is gone with the copies: asteroids' fullscreen gets the centring
check, every cabinet gets the `deleteDatabase` test (only frogger had it),
and every cabinet's blocked-storage case loads a real user set as galaga's
did. Galaga's "hwtest ROM stays silent" moved to its `audio.spec.ts`.
The `hiscore.spec.ts` blocks stay per cabinet: they depend on each board's
HIGH SCORE layout and gate.

## Low priority

- ~~**CLAUDE.md port list.**~~ Done: `8110` and `8400`/`8410` added.
- ~~**ibmpc-at typing helpers wait on time, not the guest.**~~ Done: the
  8042's `keys_pending()` (FIFO, link transfer or unread port 0x60 byte) is
  exposed as `keysPending`, and `tap`/`typeStr` wait on it. No JS queue is
  needed, the keyboard buffers make and break itself.
- ~~**POKEY linked low channel.**~~ Done: a linked low channel borrows at
  its own AUDF, then every 256 clocks, and the high channel's borrow reloads
  both (MAME `pokey.cpp` `step_one_clock`).
- ~~**actionlint isn't run.**~~ Done: `make lint` runs a pinned,
  SHA-256-checked actionlint 1.7.12 (`fetch-actionlint.sh`, cached in
  `retroweb/.tools/`). Clean with and without shellcheck on PATH.
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
2. ~~**M2**, one dev server.~~ Done, see M2.
3. ~~**M3**, shared arcade specs.~~ Done, see M3.
4. **Low**: ~~the CLAUDE.md port list~~, ~~the ibmpc-at typing helpers~~,
   ~~actionlint in `make lint`~~, ~~the POKEY linked low channel~~ (done, see
   Low). The PC front-end overlap waits for M1's worker move.
