# Centipede Arcade — review notes

Atari Centipede (1981) board emulator under `retroweb/centipede/`.
Fidelity rules follow `CLAUDE.md` and `.claude/arcade.md`.

## Sources

- Atari DP-182 schematic package, 1st printing (sheet 1B microprocessor,
  address decoder, memory map table; sheet 2B player, option and trackball
  inputs, video output, audio output, high score memory). Scans from the
  archive.org arcade manual collection.
- Atari Centipede operator's manual (fig. 7 game options, fig. 8 price
  settings).
- Atari's Centipede source as published in the `historicalsource/centipede`
  repository: `CENDEF.MAC` (memory map, switch polarities, IRQ rate),
  `CENTST.MAC` (colour RAM polarity), `CENTIP.DOC` (design notes). Read for
  behaviour only; nothing from those files is in this repo.
- General Instrument 1983 Electrically Alterable Non-Volatile Memory
  Handbook, ER2055 data sheet (bitsavers).
- Atari POKEY data sheet C012294 (`shared/atari/pokey.*`).

## Board

- 6502 at 12.096 MHz / 8. H3 and J2 decode A13-A10, so A14/A15 mirror.
- Memory map per the DP-182 table: RAM `$0000`, playfield `$0400`-`$07BF`,
  motion objects `$07C0`-`$07FF`, option switches `$0800`/`$0801`, inputs
  `$0C00`-`$0C03`, POKEY `$1000`, colour RAM `$1404`/`$140C`, EAROM
  `$1600`/`$1680`/`$1700`, IRQ acknowledge `$1800`, output latch `$1C00`,
  watchdog `$2000`, trackball clear `$2400`, program `$2000`-`$3FFF`.
- IRQ: D4 LS74 latches 32V on each 16V edge into /IRQ, four per frame
  (`CENDEF.MAC`); `$1800` (IRQRES) clears it.
- L4 LS107 stretches PHI0 while the playfield RAM is selected.
- Watchdog: L2 LS90 resets the board at its 8th count unless `$2000` is
  written.
- Colour RAM is active low (`$F` is black, `CENTST.MAC`); bit 3 is the
  alternate bit that makes a paler blue or green (sheet 2B video output).
- Option switch factory values `0x54` (English, 3 lives, 12000 bonus,
  easy, 1-credit minimum) and `0x02` (1 coin 1 play), the manual's
  suggested settings.
- AUD into J10, held at +5 V with R69 3.3K || C53 0.01 µF feedback, then
  C65 0.22 µF into R81 100K (sheet 2B).

## EAROM

ER2055 (64×8) at E5 (sheet 2B): J4 latches the address and H4 the data on
`$1600` writes; E4 LS175 at `$1680` drives CK = D0, C1 = /D1, C2 = D2,
CS1 = D3, CS2 grounded. Per the GI data sheet, C1 = 0 and C2 = 1 erases,
C1 = C2 = 0 writes, C1 = 1 reads after a clock pulse, and the chip acts on
a mode change, clock or chip-select edge. The game erases a cell, then
writes it. IndexedDB mirrors the image so a refresh keeps scores the way
the chip does across power. Reset HIGH SCORE clears the saved image and
resets the board.

## Unsourced

- An erased cell reads `$FF` and a write can only clear bits; the data
  sheet says only that erased cells hold no valid data until written.
- PF stretching doubles the CPU cycle; the exact stretch from the L4 J/K
  gating isn't worked out.
- The L2 watchdog clock is taken as once per frame; its source line runs
  off the cropped sheet.
- Frame timing (256 lines at 60 Hz, 240 visible) and the tile/motion
  object flip bits, transparent pen rule and 8-pixel cocktail clip are
  kept from earlier work and not yet traced on the scan.
- Trackball X counts down for rightward motion; the quadrature phase
  order isn't legible on the scan.

## Departures (labelled)

- High-score EAROM image persists in IndexedDB.

## ROM policy

Ships a from-scratch hwtest ROM only. User sets (the standard `centiped`
dump zip or loose `136001-307`…`310` program chips plus `136001-211` /
`212` gfx) load into IndexedDB. Size-checked; CRC labels hwtest vs user
only.

## Tests

- `make -C retroweb/shared/cpu test-smoke-mos6502` + `dormann`
- `make -C retroweb/centipede check` (machine, POKEY, board smoke)
- `make -C retroweb/centipede/web test` (Playwright on :9100)
