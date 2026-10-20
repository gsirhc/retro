# Missile Command Arcade — review notes

Atari Missile Command (1980) board emulator under `retroweb/missile/`.
Fidelity rules follow `CLAUDE.md` and `.claude/arcade.md`.

## Sources

- Atari DP-147 schematic package, 2nd printing (sheet 1B microprocessor,
  sync chain, memory map; sheet 2B input and output, POKEY, audio).
  Scans from the arcarc.xmission.com Atari manual collection.
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

- Loaded at SYNC when the opcode's low five bits are `00001` (every
  `($zz,X)` instruction) and /IRQH is inactive. It goes high on the
  instruction's data access. The shared core skips the zero-page dummy
  read, so that is its fifth bus access.
- Pixel address is the 16-bit pointer. Data comes back in bits 7–6, and
  bit 5 for the 3rd colour at `$E000+`. Writes go through the 035826-01
  L6 masks. Without that PROM the board keeps every pixel but the one
  addressed.
- A 3rd-bit access takes one extra CPU cycle.

## Video

- 256 × 231 shown, lines 25–255. The rev 3 listing describes that
  picture; line 24 is not drawn. 2bpp at `y × 64`, nibble-split. Lines
  224–255 add the 3rd colour bit from `$0200–$05FF` (memory map).
- Palette `$4B00–$4B07`: bits 3/2/1 = R/G/B, active low.
- FLIP (OUT0 bit 6 low) reverses V. IRQ edges, the half-rate window and
  VBLANK follow the flipped count. VBLANK in this model is V < 24.

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
- VBLANK's gate (8V, 16V, 32V, 64V, 128V into A5) doesn't read as a
  line count on the scan. The model uses V < 24, and the page starts at
  line 25, so the picture is the rev 3 listing's 231 lines.
- The watchdog at `$4C00` is 8 frames. Its clock source runs off the
  cropped sheet.
- The half-rate window is the 3INH cities area. The gate that stretches
  Φ0 there isn't legible on the scan.
- The 3rd-bit address mix places that plane in `$0200–$05FF`, matching
  the rev 3 layout (line 199 at `$0200`). The permutation isn't traced
  on sheet 2A.

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
