# 486 DX2-66 -- parity backlog

The open gaps between this machine and a real 1993-94 DX2-66 board, plus
the test gaps against the repo rules in `CLAUDE.md`. It collects what
`PC486_REVIEW.md` §39.5 and §40.7 left open, checked against the code as
of commit `b7eca16`. When an item is fixed, write it up in
`PC486_REVIEW.md` as usual (fact, why it matters, what it fixed, source)
and tick it here with the section number.

Rough parity today: **~82%**.

| Area | Parity | Biggest gap |
|------|--------|-------------|
| CPU (ISA, PM, paging, V86, FPU) | 93% | reset state, small decode edges |
| Timing | 70% | no cache, bus or wait-state model |
| VGA | 70% | text layout, split screen, panning, aspect |
| Chipset (PIT, PIC, I/O ports) | 70% | PIT one-shot modes, no COM/LPT/game port |
| Storage (IDE, floppy) | 80% | minimal ATA command set |
| SB16 / OPL3 / CD-DA | 90% | no game port |
| Keyboard, mouse, RTC | 93% | two keyboard commands |

Every fix lands with its native test and, where it's visible from the
page, a Playwright test, in the same commit.

## 1. Chipset

- [x] **P1. PIT modes 0, 1, 4, 5 are square waves.** `pit8253.cpp` runs
  every mode as mode 3, so the one-shot modes fire periodically. Games and
  timing loops that program mode 0 and poll OUT get the wrong answer.
  Model each mode's gate, reload and OUT behaviour. Source: Intel 8254
  data sheet, "Mode Definitions". Done in §41.
- [x] **P2. PIT mode 3 doesn't count by two.** Real mode 3 decrements by 2
  per clock and splits odd counts high/low. Visible to anything that reads
  the counter back (latched or live). Same source. Done in §41.
- [x] **P3. No 8254 read-back command.** `pit8253.cpp:43` drops control
  word `11xxxxxx`. Add latched count and status read-back. Same source.
  Done in §41.
- [x] **P4. No PIC poll command.** OCW3 with P=1 should return the highest
  pending IRQ on the next read. Source: Intel 8259A data sheet, OCW3.
  Done in §41.
- [ ] **P8. Port 61h bit 4 isn't driven by channel 1.** `Chipset::service`
  flips the refresh toggle on every service pass. On an AT it follows
  the refresh request from channel 1's OUT (about every 15 us with the
  BIOS's count of 18), and BIOS delay loops count its edges. Source: IBM
  PC/AT Technical Reference, port 61h.
- [ ] **P5. No serial ports.** COM1 (3F8h, IRQ4) and COM2 (2F8h, IRQ3) as
  16550A UARTs. Opens up a serial mouse and null-modem play. Needs a
  front-end story for what's on the other end. Source: NS16550A data
  sheet.
- [ ] **P6. No parallel port.** LPT1 at 378h, IRQ7, with status bits that
  read "no printer" (or a capture-to-file printer). Source: IBM PC/AT
  Technical Reference, parallel adapter.
- [ ] **P7. No game port.** 201h on the SB16, the 558 one-shot timing
  model, and a Gamepad API / keyboard mapping on the page. Source: SB16
  hardware reference; IBM Game Control Adapter technical reference.

## 2. VGA

- [ ] **V1. Text is always 25 rows.** `ega_render.cpp:29` hard-codes
  `rows = 25`. Derive rows from Vertical Display End and Max Scan Line so
  43- and 50-line modes work. Source: IBM VGA Technical Reference, CRTC.
- [ ] **V2. Text cells are 8 dots wide.** VGA text is 9 dots, with column 9
  copying column 8 for C0h-DFh when AR10 bit 2 (line graphics) is set,
  and 8 dots when SR01 bit 0 is set. Same source, Sequencer and Attribute
  Controller.
- [ ] **V3. Attribute bit 7 is dropped.** AR10 bit 3 selects blink (with
  the CRTC's blink counter) or bright backgrounds. `ega_render.cpp:70`
  masks it off. Same source.
- [ ] **V4. Character map select (SR03) is ignored.** Two fonts at once,
  and 512-character mode via attribute bit 3. Same source.
- [ ] **V5. No line compare.** CRTC 18h (plus overflow bits) resets the
  scan-out address mid-frame for split screens. Same source.
- [ ] **V6. No pel panning.** AR13 and CRTC preset row scan (08h) drive
  smooth scrolling. Same source.
- [ ] **V7. Aspect ratio.** The page shows each mode at its pixel ratio. A
  VGA monitor fills 4:3, so 320x200 and 640x400 are drawn too short.
  `setFrameSize` in `app.js` sets `aspectRatio = w / h`. Display at 4:3
  and keep the integer pre-scale from b7eca16 on each axis.

## 3. CPU

- [ ] **C1. Reset state.** EDX should hold the 486 DX2 signature
  (`043xh`: family 4, model 3; pick and cite a stepping) and CR0 should be
  `60000010h`. Source: Intel486 Microprocessor Data Book, "Reset".
- [ ] **C2. FXCH doesn't swap tags or signal underflow.** Source: Intel SDM,
  FXCH.
- [ ] **C3. LOCK on a non-lockable instruction doesn't raise #UD.** Source:
  Intel SDM, LOCK.
- [ ] **C4. No 15-byte instruction length limit.** Over-long prefix runs
  should #GP. Source: Intel SDM Vol. 3, instruction length.
- [ ] **C5. No real-mode segment-limit #GP.** A word access at offset FFFFh
  or past 64KB should fault (#GP, or #SS for stack). §6.2 chose not to;
  the cached-limit mechanism has to keep unreal mode and HimemX working.
- [ ] **C6. REP under single-step.** TF traps after the last iteration;
  a real 486 traps after each one. Source: Intel SDM, REP and TF.

## 4. Timing

- [ ] **T1. Cache and bus timing.** Every access is charged at the L1-hit
  cost. Needs an 8KB 4-way write-through L1, a decision on L2 for a 1993
  board, 33 MHz bus cycles on a miss, and wait states for ISA I/O, the VGA
  aperture and ROM. On the hottest path. Needs its own design pass and
  period board numbers before any code (§39.5, §14-§16).

## 5. Storage

- [ ] **S1. Minimal ATA command set.** `wd1003.cpp` handles only IDENTIFY,
  INITIALIZE PARAMETERS, READ and WRITE. Add VERIFY, SEEK, READ/WRITE
  MULTIPLE (with SET MULTIPLE MODE), SET FEATURES, EXECUTE DIAGNOSTIC,
  RECALIBRATE and the power commands. Source: ATA-1 (X3.221) and the
  WD Caviar AC2250 manual.
- [ ] **S2. FORMAT TRACK leaves old sector data.** It should fill the
  track's sectors with the format filler byte. Source: NEC uPD765 data
  sheet, FORMAT A TRACK.

## 6. Keyboard

- [ ] **K1. `EE` (echo) answers ACK.** It should answer `EE`. Source: IBM
  PS/2 Technical Reference, keyboard commands.
- [ ] **K2. `F0 00` returns no scan-code set.** It should ACK and then
  return the current set. Same source.

## 7. Test parity

- [ ] **X1. Smoke doesn't prove a boot.** `web/tests/smoke.spec.ts` checks
  real-speed pacing and main-thread blocking, and says it "only needs the
  machine running". `CLAUDE.md` wants the machine to boot plus one real
  interaction or paint proof. Add one case: boot to `C:\>` and echo a
  typed key.
- [ ] **X2. Thin native suites.** pcspeaker 6, pit 9, pic 11, fdc 11,
  machine 12 tests. P1-P4 and S2 grow pit, pic and fdc anyway. Machine
  needs IRQ routing, shutdown reset and the interrupt shadow covered
  through the whole board, not just the CPU.
- [ ] **X3. Review entries missing for two fixes.** 22fafe9 (Duke Nukem 3D:
  Input Status 1 display-enable bit, CRTC 01h/12h timing) and b7eca16
  (integer pre-scale on the screen canvas) have tests but no write-up in
  `PC486_REVIEW.md`.
- [ ] **X4. Refresh the coverage numbers.** The last recorded figures are
  93.0% lines and 79.3% branches (§39.4). Re-run `make coverage` and
  `make -C web coverage` once this backlog is under way.

Already in line with the rules, for reference: every control in
`index.html` has a Playwright test; the `?test=1&fast=1` override is
gated in `app.js` and the smoke file keeps a real-speed check; deploy CI
runs `pc486-test` / `pc486-web-test` smoke only, and `pm-check` /
`vbe-check` stay in `make check`.
