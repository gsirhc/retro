# Centipede Arcade — review notes

Atari Centipede (1981) board emulator under `retroweb/centipede/`.
Fidelity rules follow `CLAUDE.md` and `.claude/arcade.md`.

## Sources

- Atari DP-182 schematic package, 5th printing (sheet 1B microprocessor,
  address decoder, memory map table; sheet 2A playfield address selector,
  playfield multiplexer, picture ROMs, motion object vertical and
  horizontal; sheet 2B player, option and trackball inputs, video output,
  audio output, high score memory). archive.org item
  `arcademanual_Centipedesp`.
- Signetics Bipolar Memories data book (Feb 1975), 82S25 16x4 RAM: open
  collector outputs return the complement of the stored bit (bitsavers).
- Atari Centipede operator's manual (fig. 7 game options, fig. 8 price
  settings).
- Atari's Centipede source as published in the `historicalsource/centipede`
  repository: `CENDEF.MAC` (memory map, switch polarities, IRQ rate),
  `CENTST.MAC` (colour RAM polarity), `CENTIP.DOC` (design notes),
  `CENTI.MAC` (object parking and the tile-to-object formula). Read for
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
- Colour RAM C8 is an 82S25, so its outputs are the complement of the
  stored nibble. Bits 0-2 light red, green and blue when 0, which is why `$F`
  is black. Bit 3 (alternate) is latched by A8 and gates Q5 and Q4 (sheet 2B).
  Each gun line is a 7407 into R66/R62/R64 220 up and R65/R61/R63 330 down.
  With the alternate bit set, Q5 sinks the blue line through R102 56 and CR1
  while blue is lit, and Q4 sinks the green line through R101 56 and CR2
  while blue is dark (A8 pin 11 and pin 10 gate the two emitters). The
  dimmed level is the Thevenin solution of that stage, about 0.54 of full.
- Playfield RAM address is 8H-64H and 16V-128V through P5 and P7 (sheet 2A),
  so a tile is `$0400 + 32 * row + column`. The four RAM lanes at one
  address hold the picture, vertical, horizontal and colour bytes of the
  motion objects at `$07C0` (AB4 and AB5 pick the lane).
- Picture ROMs F7 (plane 0) and H/J7 (plane 1): A3-A0 are MGA3-MGA0, A8-A4
  are PIC5-PIC1, A9 is 256H, A10 is /256H gated with PIC0. Playfield
  tiles therefore start at ROM `$0200` with PIC0 on A3. F8/H8 and the LS166
  shift out D7 first, or D0 first when PIC6 is set. PIC7 inverts MGA0-MGA2
  through E7, but MGA3 only for motion objects (B7 gates it with /256H).
- Motion objects: H6/F6 add V to the vertical byte, A7 matches when the sum
  is `$Fx`, and the low nibble is the picture line. The horizontal byte
  loads counters A5/B5, which address the 256 x 2 bit line buffer A6/B6
  and wrap at 256. C7 selects one of the three 2-bit fields of the colour
  byte by the pen. Pen 0 feeds the latched GRY value back, leaving the
  buffer alone. Field 0 does write, and E8/C5 then fall back to the
  playfield pen. Colour RAM address is `4 + pen` for the playfield and
  `12 + GRY` for objects (D8, E8). Objects are written in slot order, so a
  later slot wins.
- Object window: `CENTI.MAC` parks objects at VPOS `$F8` and tests
  `VPOS >= $F8` as "off top of screen". Centipede segments and the flea
  enter there with live pictures. `CKSET` sets FLIP and `CKF8 = $F8`
  together, so the cocktail flip parks at 0 instead. A real machine
  therefore shows nothing at buffer `$F8`-`$FF`, or `$00`-`$07` with FLIP
  set, and that band is hidden.
- Object alignment: the mushroom explosion code in `CENTI.MAC` places an
  object over playfield cell `$400 + 32 * row + col` at VPOS `col * 8` and
  HPOS `(31 - row) * 8 - 4` ("lower left corner is F8", "explosion is
  centered in picture"). X is the line buffer address and an object covers
  lines `240 - HPOS` to `255 - HPOS`, which puts that 16-line picture
  centred on the tile to the pixel.
- Option switch factory values `0x54` (English, 3 lives, 12000 bonus,
  easy, 1-credit minimum) and `0x02` (1 coin 1 play), the manual's
  suggested settings.
- AUD into J10, held at +5 V with R69 3.3K || C53 0.01 µF feedback, then
  C65 0.22 µF into R81 100K (sheet 2B).

## EAROM

ER2055 (64×8) at E5 (sheet 2B): J4 latches the address and H4 the data on
`$1600` writes; E4 LS175 at `$1680` drives CK = D0, C1 = /D1, C2 = D2,
CS1 = D3, CS2 grounded. Traced on sheet 2B at 500 dpi: D0 goes to
Q1 (pin 2) to E5 CK, D1 to /Q4 (pin 14) to C1, D2 to Q3 (pin 10) to C2, D3
to Q2 (pin 7) to CS1. The C1 net also drives H4's /OC, so H4 releases
the data lines on a read. Per the GI data sheet, C1 = 0 and C2 = 1 erases,
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
- Frame timing (256 lines at 60 Hz, 240 visible).
- The gate that blanks objects in the parked band is not found on DP-182
  (5th printing, sheets 1B, 2A, 2B) or DP-189 (1st printing, sheet 1B, the
  only sheet scanned); the band itself comes from the game code above. `CENIRQ.MAC` adds 1 to HPOS whenever it sets PIC7 to turn
  a picture; the PIC7 line flip as drawn doesn't account for that pixel.
- Alternate-bit level uses typical device drops: 1N914 0.8 V, 2N3904
  VCE(sat) 0.15 V, 7407 VOL 0.3 V, and an unloaded monitor input.
- Cocktail flip is not applied to the picture.
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
