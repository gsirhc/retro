# 486 DX2-66 -- parity backlog

The open gaps between this machine and a real 1993-94 DX2-66 board, plus
the test gaps against the repo rules in `CLAUDE.md`. When an item is
fixed, write it up in `PC486_REVIEW.md` as usual (fact, why it matters,
what it fixed, source) and delete it here. Done so far: the PIT and PIC
(§41) and the VGA (§42, §43).

Rough parity today: **~86%**.

| Area | Parity | Biggest gap |
|------|--------|-------------|
| CPU (ISA, PM, paging, V86, FPU) | 93% | reset state, small decode edges |
| Timing | 70% | no cache, bus or wait-state model |
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

- **C1. Reset state.** EDX should hold the 486 DX2 signature
  (`043xh`: family 4, model 3; pick and cite a stepping) and CR0 should be
  `60000010h`. Source: Intel486 Microprocessor Data Book, "Reset".
- **C2. FXCH doesn't swap tags or signal underflow.** Source: Intel SDM,
  FXCH.
- **C3. LOCK on a non-lockable instruction doesn't raise #UD.** Source:
  Intel SDM, LOCK.
- **C4. No 15-byte instruction length limit.** Over-long prefix runs
  should #GP. Source: Intel SDM Vol. 3, instruction length.
- **C5. No real-mode segment-limit #GP.** A word access at offset FFFFh
  or past 64KB should fault (#GP, or #SS for stack). §6.2 chose not to;
  the cached-limit mechanism has to keep unreal mode and HimemX working.
- **C6. REP under single-step.** TF traps after the last iteration;
  a real 486 traps after each one. Source: Intel SDM, REP and TF.

## 4. Timing

- **T1. Cache and bus timing.** Every access is charged at the L1-hit
  cost. Needs an 8KB 4-way write-through L1, a decision on L2 for a 1993
  board, 33 MHz bus cycles on a miss, and wait states for ISA I/O, the VGA
  aperture and ROM. On the hottest path. Needs its own design pass and
  period board numbers before any code (§39.5, §14-§16).

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
- **X2. Thin native suites.** pcspeaker 6, fdc 11, machine 12 tests (pit
  and pic grew to 33 and 14 in §41). S2 grows fdc anyway. Machine
  needs IRQ routing, shutdown reset and the interrupt shadow covered
  through the whole board, not just the CPU.
- **X3. Review entries missing for two fixes.** 22fafe9 (Duke Nukem 3D:
  Input Status 1 display-enable bit, CRTC 01h/12h timing) and b7eca16
  (integer pre-scale on the screen canvas) have tests but no write-up in
  `PC486_REVIEW.md`.
- **X4. Refresh the coverage numbers.** The last recorded figures are
  93.0% lines and 79.3% branches (§39.4). Re-run `make coverage` and
  `make -C web coverage` once this backlog is under way.

Already in line with the rules, for reference: every control in
`index.html` has a Playwright test; the `?test=1&fast=1` override is
gated in `app.js` and the smoke file keeps a real-speed check; deploy CI
runs `pc486-test` / `pc486-web-test` smoke only, and `pm-check` /
`vbe-check` stay in `make check`.
