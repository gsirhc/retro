# Missile Command Arcade — review notes

Atari Missile Command (1980) board emulator under `retroweb/missile/`.
Fidelity rules follow `CLAUDE.md` and `.claude/arcade.md`.

## Sources

- Atari DP-147 schematic package, 1st printing (sheet 1B microprocessor,
  address decoding and sync; sheet 2A video generator with its signal
  glossary; sheet 2B memory map, inputs and outputs). archive.org item
  `missile-command-dp-147-01-1st-printing-sheet-1-side-a`. The 2nd printing
  scans (arcarc.xmission.com) cover POKEY and audio.
- Signetics Bipolar Memories data book (Feb 1975), 82S25: outputs are the
  complement of the stored bit (bitsavers).
- Atari TM-147 operator's manual, 2nd printing (fig. 7 option switches,
  self-test legend). The Regulator/Audio II board (035435) is a pair of
  TDA2002AV amplifiers, gain of ten.
- Andy McFadden's rev 3 disassembly,
  https://6502disassembly.com/va-missile-command/MissileCommand.html
  (high-score RAM, R8/R10 decode, IRQ rate, trackball deltas, 256×231
  picture). Read for behaviour only.

## Clock

- 10 MHz crystal (sheet 1B). Pixel clock 5 MHz. Horizontal counters
  through 128H, vertical counters 1V through 128V: 320 clocks by 256
  lines, 15.625 kHz lines, 61.035 Hz frames.
- CPU is 10 MHz / 8 = 1.25 MHz, except the last 32 lines. 3INH is low
  there (NAND of 128V, 64V and 32V, sheet 1B), the cities area, and the
  CPU runs at half rate. One frame is 224 × 80 + 32 × 40 = 19200 cycles,
  so the average is 1,171,875 Hz. The page paces at that average.
- POKEY stays at 1.25 MHz. Audio is mixed per scanline.

## Interrupts

- F7 latches 32V. Its clock is 16V, or /16V when FLIP is high (sheet 1B),
  so /IRQ asserts at V = 0, 64, 128, 192 and holds until `$4D00`.
- E7 clocks the previous /IRQ on SYNC, which the board calls /IRQH. Both
  are sampled at instruction boundaries. The 6502 sees the pin from the
  previous SYNC.
- Reset clocks the V = 0 edge, so the first instruction can see /IRQ.

## MADSEL

- R4 counts five phi0 cycles after SYNC when the opcode's low five bits are
  `00001` and /IRQH is inactive (sheet 2A). MADSEL is then high for the data
  access of a `($zz,X)` instruction. The shared core skips the zero-page
  dummy read, so that is its fifth bus access.
- Address: with MADSEL high the L2/K2/J2/H2 muxes give MAD13-MAD0 =
  BA15-BA2, so a byte holds four pixels. MUSHROOM (BA15, BA14 and BA13 all
  high, N3) switches M2, M3 and L3: MAD13-MAD11 are 0, MAD10 = BA11,
  MAD9 = /BA11, MAD8-MAD1 = BA10-BA3 and MAD0 = BA12. Those bytes land in
  `$0200-$05FF`.
- Write: MD0-MD3 take D6 and MD4-MD7 take D7 for two colours, every MD takes
  D5 for the third colour (data selector muxes F5, H5, J5, K5). L6
  (035826-01) is addressed by MADSEL, MUSHROOM and BA2-BA0 and its low
  outputs enable chips WP0-WP7. The schematic prints its table: rows 0-15
  all chips, 16-23 chips n and n+4 for pixel BA1-BA0, 24-31 one chip per
  BA2-BA0. The default PROM is that table. A user PROM replaces it.
- Read: P7 picks bit BA1-BA0 of each nibble and E7 latches them onto D7
  (high nibble) and D6 (low nibble). A LS251 puts DRAM bit BA2-BA0 on D5.
- Above `$E000` the access runs a two-colour cycle and then a third-colour
  cycle, and phi0 is extended one cycle (phiEXTEND, sheet 2A).

## Video

- 256 x 231 shown, lines 25-255. VBLANK is 1 for V < 24 (NOR of 32V, 64V,
  128V and 16V.8V). The blanking latch L8 is cleared by VBLANK*, which also
  goes low at V = 24 (B7/B5/J6, sheet 1B), so the first shown line is 25.
- 2bpp at `y * 64`. N6 latches the byte, N7 shifts the high nibble and M6
  the low nibble out as COLNUM2 and COLNUM1, bit 4+n and bit n for pixel n.
  P6 shifts the third-colour byte out as COLNUM0 during 3INH low, V >= 224.
  The three bits address the colour RAM through M7.
- Palette `$4B00-$4B07`: L7 is an 82S25, so BD3, BD2, BD1 light red, green
  and blue when 0. BD0 goes to L8 pin 5 and is not a gun.
- FLIP (OUT0 bit 6 low) makes the V counters count down, so the same rows
  show bottom up. IRQ edges, the half-rate window and VBLANK follow the
  flipped count. VBLANK in this model is V < 24.

## I/O

Memory map on sheet 1B, decode checked against the rev 3 listing.

- `$4800` read: IN0, or with CTRLD (OUT0 bit 0) the trak-ball counters
  `VVVVHHHH`. Cocktail player 2's ball is unmapped and reads 0. A
  positive vertical delta is upward roll (`$7BE6`), so the page's up
  key counts that nibble up.
- `$4900` IN1: VBLANK, self-test, slam, and the three fire buttons, the
  switches active low.
- R8 is POKEY ALLPOT (`$4008`). The program inverts it. With On = 0
  after that invert, city bits `00` are 6 cities and bonus bits `000`
  are every 10,000 (`$5B08`, `$607B`). Switch 4 off is the mini
  trak-ball: the self-test draws `F`, which TM-147 says is switch 4 off.
- R10 is `$4A00`, a closed switch reading 0. Factory `$73` / `$81` is 6
  cities, bonus every 10,000, bonus credit, upright, mini trak-ball, 2
  coins per play, both mechs ×1, English. TM-147's suggested marks cover
  the cities, the 10,000 bonus and the 2-coin price. The suggested
  trak-ball there is the large one (switch 4 on); this page ships the
  mini setting.

## Sound

Sheet 2B, POKEY pin 37. R38 is 10K to +5 V and C70 is 0.1 µF to ground,
into N10 (LM324) as a follower. R100, 10K, feeds AUD1. A second section
of N10, R71 and R70 both 10K, inverts that for AUD2.

## Unsourced

- The 10 Hz high-pass and the ÷4 level fit on the way out to the page
  are not on sheet 2B. They stand in for the Regulator/Audio II input.
- The watchdog at `$4C00` is 8 frames. Its clock source runs off the
  cropped sheet.
- The half-rate window is the 3INH cities area. The gate that stretches
  Φ0 there isn't legible on the scan.
- Order of the two cycles above `$E000` (two-colour first), and the
  level of D4-D0 while MADSEL is read (taken as 1, floating TTL).
- The display address is taken to match the processor address
  (VAPORIZE9-VAPORIZE12 and 3COLSEL checked, the rest not).
- The data selector table printed on sheet 2A lists MD0-MD7 as D7-D0.
  The wiring and the read path agree with each other and not with the
  table's column order, so the wiring is used.

## Departures (labelled)

- High scores live in work RAM `$002C–$005B` (`hscore_initials`,
  `hscore_scores`). A real board loses them at power-off. IndexedDB
  keeps them for a user ROM. Reset HIGH SCORE deletes the save and power
  cycles the board.
- A watchdog bite resets the CPU, POKEY and IRQ latches but keeps DRAM.
  The page's power cycle clears DRAM.

## ROM policy

Ships a from-scratch hwtest ROM only. User sets (a `missile` zip, or the
six loose `035820-02`…`035825-02` chips, optional `035826-01` PROM) load
into IndexedDB. Size-checked; CRC labels hwtest vs user only.

## Tests

- `make -C retroweb/shared/cpu test-smoke-mos6502` + `dormann`
- `make -C retroweb/missile test` (timing, IRQ, MADSEL, I/O, video, audio,
  board smoke + help screen)
- `make -C retroweb/missile/web test-smoke` (Playwright smoke on :9200)
