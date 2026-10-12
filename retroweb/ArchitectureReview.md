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
- **8253 PIT.** ibmpc-at counts channel 1 refresh edges; pc486 dropped them
  (see H2).

**Fix:** a `shared/pc/` library parameterised by the genuine hardware
differences (RTC 64 vs 128 bytes, default date, and so on), so a fidelity fix
lands once.

### H2. pc486 port 61h bit 4 (refresh toggle) is likely wrong

- `pc486/chipset.cpp:186` flips the bit once per `service()` pass, and
  `chipset.cpp:109` reopens the service gate on every port read.
- A BIOS or driver delay loop polling `in al,61h` therefore sees a flip on
  nearly every read instead of every ~15 µs (PIT channel 1), so
  refresh-calibrated delays run far too short.
- ibmpc-at does it right (`ibmpc-at/chipset.cpp:153`).
- The only test, `EXPECT_NE(before & 0x10, after & 0x10)` in
  `pc486/tests/chipset_test.cpp:129`, passes either way. Add a test that
  measures the flip rate.

### H3. Arcade front ends are mostly copy-paste, and a fix already didn't propagate

- `crc32`, `unzip`, `idb`, and `idbGet` are byte-identical in all 7 `app.js`
  files. frogger, scramble, and galaxian `app.js` are about 85% identical.
- The audio worklet, hiscore persistence, DIP persistence, ROM ingest, and
  tick loop repeat with small variations.
- **Drift bug.** galaga's `idbSet` resolves on `tx.oncomplete`. The other six
  resolve on the request's `onsuccess`, which fires before the transaction
  commits, so a fast reload can lose the write. galaga's `rom.spec.ts` polls
  IndexedDB to work around exactly this.

**Fix:** `shared/arcade-core.js` covering ROM ingest, IndexedDB, the audio
worklet, hiscore/DIP persistence, and the tick loop. Each machine keeps
`roleOf`/`identifySet`, `applyKeys`, and its hiscore address and signature.

### H4. All 7 arcades freeze if IndexedDB fails

`const stored = await idbGet(ROM_KEY)` (for example
`frogger/web/app.js:479`) is unguarded. If storage is blocked, the promise
rejects before `requestAnimationFrame(tick)` and even the self-test ROM never
runs. No test covers it. Wrap the call and fall back to the built-in ROM.

## Medium priority

### M1. Hiscore tests exercise a test-only copy of the logic

- `__test.restoreHiscoreNow` (`frogger/web/app.js:566`) reimplements
  `maybeRestoreHiscore` minus its POST factory-table gate ($83F1/$83FA). The
  production gate is never exercised in a browser.
- "Reset confirm clears the saved table" clicks OK and then calls
  `resetHiscoreNow()` itself, so it can't fail if the button handler is
  broken.
- The same pattern is in frogger, scramble, and galaxian.

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
- **IndexedDB connections.** `idb()` opens a new connection on every call and
  never closes it.
- **Comment style.** About 327 comments use `--`/`—` asides (mostly pc486
  tests), `shared/theme-init.js` has an 8-line banner, and
  `pc486/web/index.html` has 2 HTML comments. All break the repo's comment
  rules.

## Suggested order

1. **H2 and H4.** Real bugs, small fixes. Add a refresh-rate test and a
   blocked-IndexedDB test.
2. **H3, with M1 and the galaga `idbSet` fix.** Extracting `arcade-core.js`
   makes the hiscore tests exercise the real code path.
3. **M2, M3, M4, M7.** Cheap, and they tighten the guardrails.
4. **H1, the `shared/pc/` merge.** The biggest job. Start with `pit8253` and
   `cmos_rtc`, which are nearly identical, then reconcile the PIC toward the
   more faithful ibmpc-at model.
5. **CI and Makefile consolidation**, whenever convenient.
