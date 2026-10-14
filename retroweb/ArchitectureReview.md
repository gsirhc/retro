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

### M2. Wall-clock pacing bands are too loose

- The accepted range is roughly ±50%: arcades 1.5–4.5 MHz for a 3.072 MHz
  clock, ibmpc-at 4–12 for 8, pc486 33–99 for 66.
- A 1.4× regression passes. The 80 ms frame clamp means the emulator can't
  legitimately run faster than real time, so the upper bound can be about
  +10% while the lower bound stays loose for slow runners.
- assembler6502 has no wall-clock pacing test at all.

### M3. The fast-test flag conflicts with CLAUDE.md wording

- "Never speed these up" says the shipped emulator never runs fast "under any
  URL… a real visitor can reach". ibmpc-at and pc486 ship `?test=1&fast=1`
  (20×), which any visitor can type.
- Either reword the rule ("no page control") or keep the flag out of the
  deployed build.
- Only Altair asserts that `window.__test` is absent without `?test`. No suite
  checks that `?fast=1` alone stays at real speed.

### M4. Site staging is a denylist and leaks dev files

- Currently shipped: `hwtest_roms.h` for all 7 arcades, `pc486/__pycache__/`,
  and `altair8800/roms/gen_roms.py`.
- `Makefile`'s `site` target repeats the same strip block 11 times. Move to an
  allowlist or a per-machine `stage` target.
- `.DS_Store` is committed at `retroweb/` and isn't git-ignored.

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

### M7. 208 TypeScript specs are never linted or typechecked

`eslint.config.js` covers `**/*.js` only, and there's no `tsc --noEmit`.
Playwright strips types without checking them.

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
3. **M2, M3, M4, M7.** Cheap, and they tighten the guardrails.
4. **H1, the `shared/pc/` merge.** The biggest job. Start with `pit8253` and
   `cmos_rtc`, which are nearly identical, then reconcile the PIC toward the
   more faithful ibmpc-at model.
5. **CI and Makefile consolidation**, whenever convenient.
