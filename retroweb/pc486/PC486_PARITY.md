# 486 DX2-66 -- parity backlog

The open gaps between this machine and a real 1993-94 DX2-66 board, plus
the test gaps against the repo rules in `CLAUDE.md`. When an item is
fixed, write it up in `PC486_REVIEW.md` as usual (fact, why it matters,
what it fixed, source) and delete it here. Done so far: the PIT and PIC
(§41), the VGA (§42, §43) and the CPU edges C1-C14 (§44-§46) and cache and bus timing (§47).

Rough parity today: **~89%**.

| Area | Parity | Biggest gap |
|------|--------|-------------|
| CPU (ISA, PM, paging, V86, FPU) | 98% | FPU arithmetic depends on the host |
| Timing | 88% | VLB video wait states are an estimate; no PCD/PWT or DMA contention |
| VGA | 92% | a split lands on a whole row in doubled modes |
| Chipset (PIT, PIC, I/O ports) | 78% | no COM, LPT or game port |
| Storage (IDE, floppy) | 80% | minimal ATA command set |
| SB16 / OPL3 / CD-DA | 90% | no game port |
| Keyboard, mouse, RTC | 93% | two keyboard commands |

Every fix lands with its native test and, where it's visible from the
page, a Playwright test, in the same commit.

# TODO

## 1. Chipset

- **P5. No serial ports.** COM1 (3F8h, IRQ4) and COM2 (2F8h, IRQ3) as
  16550A UARTs. Opens up a serial mouse and null-modem play. Needs a
  front-end story for what's on the other end. Source: NS16550A data
  sheet.
- **P6. No parallel port.** LPT1 at 378h, IRQ7, with status bits that
  read "no printer" (or a capture-to-file printer). Source: IBM PC/AT
  Technical Reference, parallel adapter.
- **P7. No game port.** 201h on the SB16, the 558 one-shot timing
  model, and a Gamepad API / keyboard mapping on the page. Source: SB16
  hardware reference; IBM Game Control Adapter technical reference.
- **P8. Port 61h bit 4 isn't driven by channel 1.** `Chipset::service`
  flips the refresh toggle on every service pass. On an AT it follows
  the refresh request from channel 1's OUT (about every 15 us with the
  BIOS's count of 18), and BIOS delay loops count its edges. Source: IBM
  PC/AT Technical Reference, port 61h.

## 2. VGA

- **V11. Split screen granularity in doubled modes.** The renderers draw
  one output line per logical row, so a Line Compare that falls on the
  second raster line of a doubled row (an odd value in mode 13h) starts
  the lower part one row late. Fixing it means rendering doubled modes at
  raster height. IBM notes 200-line split screens should use an even
  value, so period software mostly avoids it (§42.1).

## 3. CPU

- **C15. FPU arithmetic depends on the host.** Results come from the
  host's `long double`, whose significand is 53 bits on the Apple arm64
  dev machine (plain `double`), 64 bits on x86-64 CI (the real x87
  format), and 113 bits in the shipped wasm (software quad). So the
  same program can round differently in each, and none of them is
  checked against a 486. The
  fix is a software 80-bit FPU for the basic operations (add, subtract,
  multiply, divide, square root, remainder, round, conversions, precision
  control, and the PE, UE, OE, DE flags), most likely Berkeley SoftFloat
  3e's extF80 (BSD-3). Transcendentals (FSIN, FPTAN, F2XM1, FYL2X...)
  come from 486 microcode, so bit-exact results need reference vectors
  from real hardware. Deferred: period games mostly use fixed-point
  math and don't depend on the last bit, so this matters for
  diagnostics, exact-result checks and native tests that differ between
  the Mac and CI.

## 4. Timing

- **T2. Smaller timing gaps.** The VL-Bus card's wait states (1 on a
  write, 3 on a read) are an estimate; a period card's data sheet would
  settle them. The page-level PCD and PWT bits are ignored, DMA and
  bus-master cycles don't hold the bus, and page-straddling accesses and
  descriptor-table reads aren't timed (§47.4).

## 5. Storage

- **S1. Minimal ATA command set.** `wd1003.cpp` handles only IDENTIFY,
  INITIALIZE PARAMETERS, READ and WRITE. Add VERIFY, SEEK, READ/WRITE
  MULTIPLE (with SET MULTIPLE MODE), SET FEATURES, EXECUTE DIAGNOSTIC,
  RECALIBRATE and the power commands. Source: ATA-1 (X3.221) and the
  WD Caviar AC2250 manual.
- **S2. FORMAT TRACK leaves old sector data.** It should fill the
  track's sectors with the format filler byte. Source: NEC uPD765 data
  sheet, FORMAT A TRACK.

## 6. Keyboard

- **K1. `EE` (echo) answers ACK.** It should answer `EE`. Source: IBM
  PS/2 Technical Reference, keyboard commands.
- **K2. `F0 00` returns no scan-code set.** It should ACK and then
  return the current set. Same source.

## 7. Test parity

- **X1. Smoke doesn't prove a boot.** `web/tests/smoke.spec.ts` checks
  real-speed pacing and main-thread blocking, and says it "only needs the
  machine running". `CLAUDE.md` wants the machine to boot plus one real
  interaction or paint proof. Add one case: boot to `C:\>` and echo a
  typed key.
- **X2. Thin native suites.** pcspeaker 6, fdc 11, machine 13 tests (pit
  and pic grew to 34 and 14 in §41). S2 grows fdc anyway. Machine
  needs IRQ routing, shutdown reset and the interrupt shadow covered
  through the whole board, not just the CPU.
- **X3. Review entries missing for two fixes.** 22fafe9 (Duke Nukem 3D:
  Input Status 1 display-enable bit, CRTC 01h/12h timing) and b7eca16
  (integer pre-scale on the screen canvas) have tests but no write-up in
  `PC486_REVIEW.md`.
- **X4. Refresh the coverage numbers.** The last recorded figures are
  93.0% lines and 79.3% branches (§39.4). Re-run `make coverage` and
  `make -C web coverage` once this backlog is under way.
- **X5. Playwright checks that flake under load.** The front panel's
  Turbo ratio, the audio-ring checks in `tone.spec.ts` and
  `performance.spec.ts`, the keyboard boot notice, the smoke file's
  main-thread blocking check, and `cdda.spec.ts` on the shared live page
  each fail now and then in a full run and
  pass on their own (§45.5, §46.5). The smoke one gates deploys. Each
  needs its timing margin or wait condition fixed, not a retry.
- **X6. The C1-C14 CPU work costs speed.** The extra per-instruction
  checks (prefix count and LOCK, the real-mode limit, RF and breakpoint
  bookkeeping, #AC and watchpoint hooks) left the core about 8-10%
  slower per guest cycle natively than before them. The wasm measured
  about 15% slower (116 to 98 MHz host-bound over a FreeDOS boot) before
  §46.6's partial fix and hasn't been re-measured since. That shrinks the
  margin over the real 66 MHz, and a heavy game that falls under it lags
  its audio. Doom's sound effects were reported a little late after
  these changes; not yet confirmed as the cause (`?audiotrace` in Doom
  would show it). Reverting all the checks restores the old speed, so
  the fix is making each one cheaper, not removing it. See §46.6.

Already in line with the rules, for reference: every control in
`index.html` has a Playwright test; the `?test=1&fast=1` override is
gated in `app.js` and the smoke file keeps a real-speed check; deploy CI
runs `pc486-test` / `pc486-web-test` smoke only, and `pm-check` /
`vbe-check` stay in `make check`.
