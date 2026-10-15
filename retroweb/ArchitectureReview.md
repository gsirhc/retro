# retroweb Architecture Review

Audit of architecture, consistency, test practice, and code reuse across all
11 machines, `shared/`, the Makefiles, and both CI workflows. Findings come
from reading and diffing source; no tests or builds were run for this review.

## Overall

The per-machine shape is very consistent: native core + GoogleTest, wasm front
end, Playwright suite. Shared CPU cores carry strong conformance suites
(zexdoc/zexall, SingleStepTests, Dormann), and the smoke/full split is clean.
The weak spot is reuse: a lot of what's shared across machines is copy-paste,
and the copies have already drifted. Fixes land in one copy and not the
others. That's the main thing to address.

### What's working well

- Uniform machine layout, Makefile targets, fixed test ports, and Playwright
  versions/config.
- Real shared code where it exists: `shared/cpu`, `shared/galaxian` (frogger's
  forwarding headers such as `frogger/video.h` are the right pattern), and the
  theme/footer/fullscreen chrome, staged by copy rather than `../`.
- Deep native test suites on the PCs (567 and 835 TESTs) and review docs with
  citations.

## High priority

### H1. ibmpc-at and pc486 fork the whole PC chipset, and the forks diverge

About 14 files are duplicated: `pic8259`, `pit8253`, `dma8237`, `fdc765`,
`i8042`, `cmos_rtc`, `wd1003`, `ega`, `chipset`, and others. Some differ only
by namespace (`cmos_rtc.cpp` is a 10-line diff). Others have drifted to
different fidelity levels for the same chip:

- **8259 PIC.** ibmpc-at models the transparent request latch, two-cycle INTA,
  default IR7, and CAS-line cascading (`ibmpc-at/pic8259.h`). pc486 has a
  simpler edge-only `raise()`/`acknowledge()` model.
- **8253 PIT.** Both now count channel 1 refresh edges (H2), so the two
  differ only in namespace, the header comment, and pc486's
  `cycles_to_next_count()` service-gate helper.

**Fix:** a `shared/pc/` library parameterised by the genuine hardware
differences (RTC 64 vs 128 bytes, default date, and so on), so a fidelity fix
lands once.

### H2. pc486 port 61h bit 4 (refresh toggle). Fixed

- `pc486/chipset.cpp` flipped the bit once per `service()` pass, and every
  port read reopens the service gate, so a loop polling `in al,61h` saw a
  flip on nearly every read instead of every ~15 µs.
- **Fixed** (`pc486/PC486_REVIEW.md` §52). pc486's `Pit8253::tick` reports
  channel 1's rising edges as ibmpc-at's does, the chipset toggles bit 4 on
  each one, and `Machine::start_refresh()` programs channel 1 with IBM's
  POST bytes, since the Bochs BIOS never does. PARITY item P8 is closed.
- **Tests.** `ChipsetTest.RefreshToggleFollowsChannelOneNotPortReads`
  measures the flip rate (100 flips over 100 refresh periods at 66 MHz,
  polled every 40 cycles). `RefreshToggleCountsEveryRequestInOneServicePass`
  checks that two requests in one pass leave the bit unchanged. Also
  `Pit8253Test.Channel1RefreshRisesOncePerCount` and
  `MachineTest.RefreshRunsFromPowerOnAndAfterReset`. The old toggle fails
  both port 61h chipset tests.

### H3. Arcade front ends were mostly copy-paste. Fixed

`crc32`, `unzip` and the IndexedDB helpers were byte-identical in every
arcade `app.js`, and the audio worklet, hiscore persistence, DIP
persistence, ROM ingest and tick loop repeated with small variations. The
copies had drifted:

- galaga's `idbSet` resolved on `tx.oncomplete`; the other seven resolved on
  the request's `onsuccess`, before the transaction commits.
- pacman, asteroids, centipede and missile had dropped the audio `resume()`,
  the drain before the worklet starts, and the "context running" check.

**Fixed.** `shared/arcade-core.js` now holds the ROM file reader (zip, CRC),
an IndexedDB store whose writes resolve on commit and that reuses one
connection, the audio worklet (the complete variant), a HIGH SCORE
persister, the reset dialog, the ROM loader, DIP prefs and the frame loop.
All 8 cabinets use it. Each `app.js` keeps `roleOf`/`identifySet`,
`applyKeys`, its HIGH SCORE address, POST gate and factory check, and any
odd rule as a hook: Pac-Man's BCD TOP and tile painting, Centipede's
EAROM restore at load, Missile's factory table from the program ROM.
Together the eight `app.js` files went from 5,626 lines to 3,157, plus 353
in the core. It's staged like `arcade-touch.js`, listed in eslint, and
described in `.claude/arcade.md`.

### H4. Arcades froze if IndexedDB failed. Fixed

`const stored = await idbGet(ROM_KEY)` was unguarded. If storage was
blocked, the promise rejected before `requestAnimationFrame(tick)` and even
the self-test ROM never ran.

**Fixed** in all 8 arcades (missile included). The boot read falls back to
the built-in ROM. The `idbSet` after an upload no longer turns a good set
into "ROM rejected", and Remove ROMs reverts even if the delete fails. A
loaded set then runs for the session only, and the status line warns
that it won't reload. Each `rom.spec.ts` has a "with IndexedDB blocked"
block that makes `IDBFactory.open` throw and checks that the CPU runs, an
upload applies with that warning, and Remove ROMs reverts.

## Medium priority

### M1. Hiscore tests exercised a test-only copy of the logic. Fixed

`__test.restoreHiscoreNow` reimplemented the restore without its POST gate,
so the gate was never exercised in a browser, and "Reset confirm" called
`resetHiscoreNow()` itself after clicking OK.

**Fixed.** `__test` now exposes the persister's own functions, so nothing is
a test-only copy. Where the self-test ROM lets a test reach the gate, the
round trip goes through it. Frogger, Scramble and Galaga write the factory
table the way POST would. Galaxian, Asteroids and Missile wait for their
own gate, and Galaxian and Asteroids also hold it shut with a non-zero
table to prove the restore waits. Pac-Man's ~8 s gate keeps its existing
real-gate test. Reset tests click OK and check that the saved table is gone
from IndexedDB. Taking the gate out of the core's `maybeRestore` fails a
test in seven cabinets. Centipede has no frame gate (its EAROM restores at
load). An OK button that does nothing also fails.

A stress run (`--repeat-each 6 --workers 6`) also found an older race. A
spec that uploaded ROMs right after `goto` could beat the app's listeners,
and the upload was dropped (5 of 132 failed on the committed frogger code).
Every arcade `rom.spec.ts` and `hiscore.spec.ts` now waits for `__test`
first, and all 954 runs pass.

A second review pass found two more bugs, both older than the refactor:

- **Centipede rejected a good user set when storage was blocked.** Its
  EAROM restore at load read IndexedDB and threw, so the set showed "ROM
  rejected". It now runs for the session with the storage warning.
- **A save or Reset in flight when the ROM set changed used the new set's
  key.** The old table was filed under the new set, or the new set's entry
  was deleted instead. The persister now takes the key before it awaits.

New tests cover both: a user set with storage blocked (all 8), the "Stored
ROM unreadable" fallback (all 8), Remove ROMs disabling Reset (all 8), and,
in frogger as the core's home, the store giving way to `deleteDatabase`
and the two key races. Each one fails on the old code or under a mutation.
One guard isn't covered: `saveNow` skips its `lastSaved`/`onSaved`
bookkeeping when the set changed mid-save, and no test reaches that.

### M2. Wall-clock pacing bands were too loose. Fixed

The bands accepted roughly ±50%, so a 1.4× regression passed, and
assembler6502 had no pacing test. Every pacing test (8 arcades, both PCs,
Altair) now samples cycles and `performance.now()` in one page call, so
node round trips don't skew the window, and caps the rate at 1.1× the real
clock. The frame clamp means a correct build can't exceed that. Lower
bounds stay loose for slow runners. assembler6502's smoke gained a 1 MHz
pacing test. 320 stress runs of the arcade pacing and seam specs passed.

### M3. The fast-test flag reached the deployed site. Fixed

ibmpc-at and pc486 shipped `?test=1&fast=1` (20×), which any visitor could
type. `make site` now rewrites that expression to a constant `1` in the
deployed `app.js` (`strip_fast_flag.py`, which fails the build unless it
matches exactly once). `check_site.py` then fails the build if any
`.get("fast")` or `.has("fast")` is left in a staged `.js`. `make lint` runs the strip in
check mode against both PCs, so editing the expression breaks CI before it
breaks a deploy. Tests: every arcade and both PCs assert `__test` is
absent without `?test`. The arcades wait for a painted frame and the PCs
for the power LED, so the check can't pass just by running early. Both
PCs assert `?fast=1` alone exposes no seam, and `?test=1&fast=2` runs at
real speed with `__test.cpuMultiplier` 1 (a rate check alone can't catch
pc486, whose host can't reach 20× of 66 MHz), while `?test=1&fast=1` reads 20. CLAUDE.md's override paragraph describes the strip.

assembler6502 has no `__test`; it exposes `window.__machine` and
`window.__term` to every visitor for the suite and debugging. Neither is a
speed control, so they stay. A leftover `window.__testQueueLoad` (`TEMP
DEBUG`, unused) is removed.

### M4. Site staging was a denylist and leaked dev files. Fixed

The deploy shipped 14 dev files: the 8 arcade `hwtest_roms.h` headers,
`pc486/__pycache__/`, and Altair's ROM/disk/tape READMEs, fetch scripts and
`gen_roms.py`. Each `web/Makefile` now has a `stage` target that lists
exactly what ships, copied by `stage_web.sh`, which also drops source and
tooling types from inside those folders. `check_site.py` fails the build if
any dev file reaches `_site/`, or if a staged page's `src`/`href`, a JS
`fetch`/`importScripts`/`new Worker`/`locateFile` literal, or a manifest
`file` entry points at something that wasn't staged, so a `STAGE` list that
misses a runtime file fails too. `make lint` runs its fixture self-test.
`stage_web.sh` refuses a missing or relative `DEST`. Staged into a scratch directory, the new
output is the old file list minus exactly those 14, byte-identical apart
from the two PC `app.js` lines. `make site SITE=<dir>` stages anywhere.
`.DS_Store` is ignored repo-wide and the committed one is untracked.

### M5. Dev/prod parity for disk images, to verify

- `serve_nocache.py` transparently serves `*.img.gz` siblings; GitHub Pages
  won't.
- In a local `_site`, the pc486 HDD is a 6 MB `.gz` next to a 244 MB raw
  image, and the staged pc486 tree is 1.4 GB, over Pages' documented 1 GB
  site limit.
- If production serves the raw image, real visitors download 244 MB where
  local preview sends 6. Not yet checked against production.

### M6. Shared chip libraries have no tests of their own

- `shared/cpu` has a suite. `shared/galaxian`, `shared/atari`, and
  `shared/namco` don't; their tests live inside consumer machines.
- Some AY8910/I8255 tests are duplicated across frogger's and galaxian's
  `device_test.cpp`.
- POKEY is covered only by three tests in centipede's `machine_test.cpp`.
  Centipede has 12 native tests in total, against galaga's 81.

### M7. TypeScript specs were never linted or typechecked. Fixed

`make lint` now runs `tsc` (`tsconfig.json`, strict minus `noImplicitAny`)
and eslint with typescript-eslint's recommended rules over every spec,
helper and Playwright config. The root `package.json` carries
TypeScript 6.0, `@types/node`, `@playwright/test` and
`monocart-coverage-reports`, so CI's lint job resolves every import. Getting
to zero meant typing fixes only, no behaviour changes: coverage options,
`HTMLOptionElement` casts, `Buffer` annotations, `(window as any).__machine`,
plus unused imports and stale `eslint-disable` lines. A planted type error
and a planted unused variable each fail `make lint`.

## Low priority

- **Build source lists.** Each machine lists its sources three times (wasm
  `SRC`/`HDR`, CMake `BOARD`, `COV_SRCS`), and the shared-file copy list
  repeats in 11 web Makefiles. `frogger/web/Makefile` also defines `SHARED`
  twice. A `shared/machine.mk` would remove most of this.
- **CI.** About 22 hand-copied jobs in each of the two ~850-line workflows; a
  reusable workflow or matrix would replace them. Builds aren't reproducible:
  `setup-emsdk version: latest` is unpinned, and googletest is fetched by tag
  with no `URL_HASH`, 12 times.
- **Terminal code.** Altair and assembler6502 terminal filters (`vt52Filter`,
  `adm3aFilter`, …) have identical logic; only the comments differ. xterm is
  vendored twice, byte-identical. Candidates for `shared/terminal.js` and a
  shared vendor folder.
- **Arcade wasm wrappers.** `frameBuffer()` copies each frame three times, and
  the RGBA conversion repeats in all 7 `wasm_machine.cpp` files. `hwtest_set()`
  is duplicated between `smoke_test.cpp` and `wasm_machine.cpp`.
- **Flaky-test masking.** `retries: 2` on CI hides flakes, and there are about
  150 `waitForTimeout` calls (Altair 32, pc486 28). Prefer `expect.poll`, and
  report tests that only pass on retry.
- ~~**IndexedDB connections.**~~ Fixed with H3: `arcadeStore` keeps one
  connection and closes it on `versionchange`.
- **Comment style.** About 327 comments use `--`/`—` asides (mostly pc486
  tests), `shared/theme-init.js` has an 8-line banner, and
  `pc486/web/index.html` has 2 HTML comments. All break the repo's comment
  rules.

## Suggested order

1. ~~**H2 and H4.**~~ Done. See H2 and H4 above.
2. ~~**H3, with M1 and the galaga `idbSet` fix.**~~ Done. See H3 and M1 above.
3. ~~**M2, M3, M4, M7.**~~ Done. See those entries above.
4. **H1, the `shared/pc/` merge.** The biggest job. Start with `pit8253` and
   `cmos_rtc`, which are nearly identical, then reconcile the PIC toward the
   more faithful ibmpc-at model.
5. **CI and Makefile consolidation**, whenever convenient.
