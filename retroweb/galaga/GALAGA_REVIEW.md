# Galaga arcade board — design notes

Citation trail for the Midway Galaga (1981) emulator. Primary sources:

- Midway *Galaga Parts and Operating Manual* (Oct 1981), DIP switch
  figure 3, pp. 2-4 and 2-5 (arcade-museum.com `Galaga.pdf`).
- Namco *Galaga* English operator's manual, self-test and option switch
  pages (arcade-museum.com `NamcoGalagaEng.pdf`).
- Midway Galaga schematic (arcade-museum.com `galaga3.pdf`), CPU board
  and video board sheets: crystal, watchdog counter, DIP multiplexers,
  07XX divider.
- Midway *Galaga Parts and Operating Manual* schematic pages 7-7 to 7-9
  (arcade-museum.com `Galaga.pdf`, PDF pp. 71-76): video PCB
  A084-91405-B508 (sprite pipeline, colour PROM network, 05XX pinout) and
  CPU PCB A084-91404-C508 with its designation list (sound section).
- National/TI LM324 datasheet (output swing); Fujitsu MB3730A, 1988
  Linear Products Data Book (input resistance).
- coinop.org GalagaMap, a 1996 memory map write-up
  (https://www.coinop.org/kdl/GameTech/GalagaMap.html).
- Fujitsu MB8840/MB8840H series datasheet, January 1987.
- Chris Lomont, *Pac-Man Emulation Guide* v0.1, for the Pac-Man hardware
  Galaga shares (tile packing, WSG).

Facts with none of these behind them are marked Unsourced below. The
behaviour is kept as is until a primary source turns up.

## 0. Status

Three Z80s, Namco video (tilemap, 64 sprites, 05XX starfield), the shared
Namco wavetable chip, and MB8843/MB8844 customs. GoogleTest (`make check`)
covers the map, a frame of video, the wavetable write, the 06XX fallback,
MB884x opcodes, and `Smoke.HwtestBootsSignatureWatchdogAndPaints`. The
Emscripten page ships a from-scratch self-test ROM. A real Midway
`galagamw` set is opt-in and browser-local. `51xx.bin` / `54xx.bin`, when
present and 1024 bytes, run the real cores; otherwise that chip stays on
the 06XX bus as a labelled fallback and `#romStatus` names the missing
file. Landing-page card and CI jobs (`galaga-test` / `galaga-web-test`,
Playwright ports 8900/8910) are wired in.

## 1. Scope

Midway `galagamw` only. Not the Namco parent, not the fast set, not the
four-CPU bootleg. Chip names: main `3200a.bin` `3300b.bin` `3400c.bin`
`3500d.bin`, sub `3600e.bin` (4K at 3J), sound `3700g.bin` (4K at 3E),
tiles `2600j.bin`, sprites `2800l.bin` then `2700k.bin`, palette
`prom-5.5n`, character LUT `prom-4.2n`, sprite LUT `prom-3.1c`, waveform
`prom-1.1d`. `prom-2.5c` is marked timing and is unused until the
schematic says what it clocks.

## 2. Clocks

Master crystal 18.432 MHz (Midway schematic, CPU board: XTAL1 18.432 MHz).

- Each Z80: 18.432 / 6 = 3.072 MHz. Live page stays at that rate.
- 51XX and 54XX: 1.536 MHz input. One instruction is that clock / 6, so
  one instruction per 12 Z80 T-states. The real core runs only after
  `$6823` releases reset and a 1024-byte image was loaded.
- Raster, same numbers as Pac-Man (Namco Pac-Man wiki, Video: 384×264,
  6.144 MHz pixel clock). Visible 288×224, about 60.606 Hz, 50688 T-states
  per frame. Galaga's own counter chain is Unsourced beyond the 07XX
  outputs on the schematic. Upright cabinet is rotated, canvas 224×288.
  No rotation control.

The three Z80s share RAM through the 08XX bus controllers. This core gives
each CPU its own slot at 18.432/6 and advances sub and sound by the same
T-state count as each main instruction, so they do not wait on each other.

## 3. Memory map

Private program ROM `$0000–$3FFF` per CPU. Shared otherwise.

- `$6800–$6807` — DIP read. The address offset selects the switch bit.
  Bit 0 of the byte is DIP 6K and bit 1 is DIP 6J (schematic: the 5K
  multiplexer on 6K drives DB0, 4K on 6J drives DB1; coinop.org GalagaMap
  agrees). A closed (ON) switch reads 0. Midway figure 3 gives every
  setting. Code defaults are 6K `0x97` (1 coin / 1 credit, bonus
  20K/70K/every 70K, 3 lives) and 6J `0xF7` (2 players, easiest rank,
  attract sound on, freeze off, rack advance off, switch 7 not used and
  open). The Midway pages don't mark which setting ships, so the
  defaults are Unsourced, though the Namco self-test page shows 1 coin /
  1 game, 3 ships and 20,000 / 70,000 bonus. Switch 7 of 6J reads 1 when
  open. The sub ROM at `$0ECA` is `LD A,($6806); AND 2; RET NZ`, so that
  bit is read there; what runs when it is clear is not traced. Switch 8 of 6J is the one-way /
  two-way coin counter in the Midway manual, while the page's Cabinet
  control sits on it. `$6800` with the defaults reads `0x03`.
- `$6800–$681F` — wavetable sound writes, the same 32 nibbles as Pac-Man,
  clocked at 3.072 MHz / 32 = 96 kHz. The generator lives in
  `retroweb/shared/namco/` so the silicon is not forked. coinop.org lists
  `$6800`+`xx00` as the sound PROM write and `xx01` as the sound RAM
  write. Galaga leaves the chip enabled; no separate enable bit was found.
- `$6820–$6827` — 74LS259. Bit 0 IRQ1 enable, bit 1 IRQ2 enable, bit 2
  sound-NMI disable (write 1 turns the NMI off; the enable is active-low
  NMION), bit 3 releases sub CPU + sound CPU + 51XX + 54XX. Power-on
  holds that bit at 0, so sub and sound stay in reset until the main
  program writes 1.
- `$6830` — watchdog reset. 8 vblanks (schematic: 74LS393 at 6A clocked by
  VBLANK, cleared by WDR, QD resets the board).
- `$7000–$70FF` / `$7100` — 06XX data and control. Control bits 0–3 are
  chip selects (bit 0 = 51XX, bit 3 = 54XX), bit 4 is read/!write, bits
  7:5 are the clock divider. A non-zero divider arms an NMI to the main
  CPU. Period is `(64 << shift)` Z80 cycles. The device clock is
  master/6/64. The control register layout, the clock divider period and
  the startup traffic (`10` + `FF`, then `71`, `A1` or `A8`, each closed
  with `10`) are Unsourced. Searched: the Midway schematic shows the 06XX
  as a block only, coinop.org says its function is unknown.
- `$8000–$87FF` — tilemap RAM. Code, then color at `+$400`.
- `$8800–$8BFF`, `$9000–$93FF`, `$9800–$9BFF` — shared work RAM, 1K banks
  mirrored through the 2K window (`& 0x3FF`). Sprite registers sit at
  offset `0x380` in each bank.
- `$A000–$A007` — 05XX starfield latches, D0. Bit 7 of the written byte
  is the flip screen.

Vblank IRQ to main and sub is gated by the IRQ1/IRQ2 latches and stays
asserted until that latch bit is written 0. The line is retried before
each `step()` so a level-high IRQ is not lost while IFF is off. Sound-CPU
NMI pulses when the vertical counter hits 64 or 192 (twice a frame, from
the 07XX divider), and only when NMION is enabled and the sound CPU is
out of reset.

## 4. Video

36×28 tile map over the 288×224 visible area. The center 32 columns are
the playfield, address `(row+2)*32 + (col-2)`. The two columns on each
side are the hcnt(8)=0 score strips: memory rows 0 and 1 on the native
right (the bottom of the upright screen) and rows 30 and 31 on the
native left (the top), walked by the vertical counter. A credit line
stored across row 1 therefore reads horizontally along the bottom.
The scan is the Pac-Man map (Lomont figure 7) with the same two-row
strips; that Galaga uses the same decode is Unsourced. A tile pen is clear when its LUT entry ORed with 0x10
is 0x1F. 2bpp nibble packing matches
the Namco character layout (high nibble plane 0, low nibble plane 1).
Sprites: 64 × 2 bytes. Each 16×16 picture is four columns at bytes
0, 8, 16 and 24. Attribute bit
0 flip H, bit 1 flip V, bit 2 2×H, bit 3 2×V (Unsourced). X is 10 bits: the position byte plus two
bits in the following register, and register 0 is 40 pixels left of the
visible origin. Y counts down from its register. A 2× sprite is the next
three codes. A sprite pen is clear when its LUT entry is 0x0F, and later sprites
overwrite earlier ones (both Unsourced). See §8 for the raster renderer and flip.

Sprite fetch, from the video PCB sheet (page 7-7):

- Y match: 3D/3C (74LS283) add the sprite's Y byte to V. 2B (LS20) with
  1F (LS32) flags the line when sum bits 7-5 are set and bit 4 is set or
  VSIZE is on, so 16 lines (32 with VSIZE). Sum bits 4-0 XOR the flip
  line (4B/4C LS86) give the row: 16V*, 8V*..1V*.
- Code: 2E latches the code. 1B (LS153) passes code bit 1, or 16V* when
  VSIZE (2C 1Q on select B); and code bit 0, or the horizontal half when
  HSIZE (2C 2Q on select A). The low code bits are replaced, not added.
- Sprite ROMs 4D (GG-L, `2800l`) and 4F (GG-K, `2700k`): A0-A2 = 1V*-4V*
  from 2C, A3/A4 = 4H/8H XOR HFLIP (3B), A5 = 8V*, A6/A7 = the 1B
  outputs, A8-A11 = code bits 2-5. Code bit 6 picks the chip through the
  3N inverter (low selects 4D).
- 3B XORs the sprite's flip bits with FLIP. The 2114 at 3H carries VSIZE,
  HSIZE, HP9, HP8 on D3-D0.
- X: 5E/5D/5C and 5J/5H/5F (LS161) load X and HP8/HP9 and count the
  line-buffer address at 6 MHz. 5C's QB (HP9's bit) is the off-buffer
  line, QC/QD are unused, so the address wraps at 1024.
- Unsourced, single values kept: V at the first visible line less the
  one-line buffer delay (15, which is `kSpriteVBias`), the buffer address
  shown at visible x = 0 (40), and the polarity of the off-buffer bit.
  The 07XX count and the read-side preset are inside customs.
- Pixel order within a ROM byte is the same nibble packing as the tiles
  (Lomont). The 02XX shifter is a custom, so this is Unsourced for
  sprites; the real ROM's pictures come out whole with it.

Colour network (J2 on page 7-7): 5N (MB7051) D0-D2 drive red through 1K,
470, 220, D3-D5 green through 1K, 470, 220, D6-D7 blue
through 470, 220. Every leg is driven high or low, so a leg's level is
its conductance over the channel's total: 33/71/151 and 81/174 of 255.
The monitor input is taken as high impedance (Unsourced). The 05XX drives
pins 11/10 (red 470/220), 9/8 (green), 7/6 (blue) onto the same nets and
its pin 20 deselects 5N, so a star sees only the 470/220 legs: 81/174 on
every channel. Shared nets with no isolation mean 5N must be three-state.

Stars are the Namco 05XX. Sources:

- Bandai Namco, CEDEC 2014 talk, slide 22 "star generator"
  (Galaxian/Galaga; reported at
  https://www.4gamer.net/games/042/G004287/20140905040/): a 16-bit
  M-sequence, period 65535, one value per dot while the screen scans,
  256x256 = 65536 dots, so each frame lands one dot later and the field
  scrolls by itself. The slide's screenshot puts the 256 columns between
  the score strips, so the field is native x 16-271 and 256 lines, 224
  shown and 32 clocked off screen.
- Taps 16, 15, 13, 4 from Xilinx XAPP052, *Efficient Shift Registers,
  LFSR Counters, and Long Pseudo-Random Sequence Generators*, Table 3.
  XOR feedback from a non-zero seed (XAPP052 uses XNOR; same period).
  `Video.StarLfsrIsMaximalLength`.
- pin4.at's 05XX page (http://www.pin4.at/pro_custom_05xx.php, prose and
  block diagram), measured on a real chip: scrolling gates the shift
  register's clock around sync, adding or dropping up to 3 clocks a
  frame; hit (output enable) and colour decode are separate stages fed
  by a 2-bit map input.
- Game ROM writes (galagamw, logged in this core): latch 5 is 0 through
  POST and 1 from the attract mode on; latches 3-4 step 00, 10, 11, 01
  every 8 frames; latches 0-2 hold 6 most of the time and alternate 6/7
  around ship entry and stage changes.

The trim lands on the last off-screen field line.
`Video.StarFieldDriftsOneDotPerFrameUnlessGated`.

Labelled departure, own design with no source: the seed (1), the hit
test (bits 15-7 = `0 1111 1111`, 128 stars a field, never two within 9
dots), the star set (bits 6-5), the colour (bits 5-0, two bits each to
red, green, blue), latch 2 meaning drop rather than add with latches
0-1 the count (6 drifts the field down one dot a frame, 7 two), latches
3-4 naming the one set of four held dark, and enable low holding the
LFSR as well as blanking it. Star positions, colours, blink and speed
therefore do not match a real cabinet. `Video.StarFieldHoldsWhileDisabled`,
`Video.StarsUseOnlyThe05xxLegs`. Draw order is stars, then sprites,
then tiles, so the score strips stay on top. Character colours OR `0x10`
into the PROM index. Palette levels come from the colour network below.

## 5. 51XX and 54XX

51XX is a Fujitsu MB8843, 54XX an MB8844. Both: 10-bit PC (1K ROM), 6-bit
data (64 nibbles). The core, `retroweb/galaga/mb88.cpp`, is written from
the Fujitsu MB8840/MB8840H series datasheet (Jan 1987); the MB8850 series
datasheet (TM335-A871) carries the same tables and gives the clean EN/DIS
bit chart. From the datasheet: the opcode map (Table 6); every
instruction's operation and its ZF, CF and ST columns (Table 5); one
cycle for one-byte instructions and two for EN, DIS, CALL, JPL and JPA;
four subroutine levels; 1K×8 ROM and 64×4 RAM; EN/DIS bits (b0 serial
IRQ, b1 timer IRQ, b2 external IRQ, b3 TO, b4 external shift clock, b5
internal shift clock, b6 TC counter, b7 internal timer); external before
timer before serial; the external request latching on the falling IRQ
edge only while enabled; IF following the pin for TSTI; TC counting on
its falling edge; the internal shift clock at 1/6 of the clock generator
(one shift per instruction cycle); output ports high during reset
(Table 2, MB8840 series fixed high). In Table 5, `↓C` and `↓Z` are read
as ST = 0 on carry or on a zero result and 1 otherwise. The MB8840
table's `·Z` in the RSTC/SETC ST column is a misprint; the MB8850 table
has `·`.

Decided from the real mask ROMs (my own disassembly of `51xx.bin` and
`54xx.bin`), not stated by the datasheet:

- External interrupt vector `$02`. Both ROMs start a K-port dispatch
  there and nothing branches to it.
- The PC carries out of a 64-byte page into the next (`54xx.bin` runs
  straight through `$03F`→`$040`), and a JMP on a page's last byte lands
  in the next page (`54xx.bin` `$0BF` must reach `$0C9`).
- Subtraction carry is a borrow. `51xx.bin` `$1DA` does `RSTC; SBC; DAS;
  STIC; LI 0; SBC`, which only works if SBC sets CF on borrow.
- DCM and DCY clear ST on borrow (the old value was 0). The 54XX's
  multi-nibble countdowns at `$046` and `$128` exit on no borrow. The
  adder reading (ST clear whenever the old value was non-zero) silences
  `Machine.UserRom54xxPlaysThroughItsDacs`.

Unsourced, kept because the real ROMs need them or don't exercise them:

- ST after reset is 1. `51xx.bin` opens with JPL at `$000`; with ST 0
  `Machine.UserRomInsertsCoinWhenLocalDumpPresent` fails.
- An instruction whose ST column is `·` sets ST to 1, untaken JMP, JPL
  and CALL included. Reading `·` literally (ST kept) fails both Play
  tests.
- Timer `$04` and serial `$06` vectors. Neither ROM enables those
  interrupts.
- Timer prescale of 32 instruction cycles. Neither ROM runs the internal
  timer (the 51XX enables the TC counter, PIO `$64`).
- Three cycles to take an interrupt, during which no instruction runs.
- RTI restores CF, ZF and ST saved at entry; RTS doesn't. Both ROMs'
  handlers restore A, X and Y by hand and not the flags.
- The timer is TH:TL, 8 bits, and VF sets on its wrap. The stack wraps
  after four levels.

CRC values `c2f57ef8` (51xx) and `ee7357e0` (54xx) are a cross-check only,
not a whitelist. CI has no dumps. The self-test and Playwright cover the
fallback. `Machine.UserRomInsertsCoinWhenLocalDumpPresent` skips unless
`roms/user/`, `GALAGA_ROM`, `~/images/arcade/galagamw.zip`, or
`~/Downloads/galagamw.zip` is present.

The fallback sits on the 06XX bus inside the core. The page maps keyboard
and gamepad onto the same active-high input bits, and the 51XX model
answers the command stream with those bits inverted on the R ports.
Switch mode returns `R0|R1<<4`, `R2|R3<<4`, `0xFF`. Credit mode returns
BCD credits, then stick nibbles, then buttons and coins; that nibble
order follows a 51XX disassembly and has not been checked against a
`galagamw` trace. Commands `03`/`04` (cocktail remap) are stored and
ignored — cocktail player 2 stays unmapped and is labelled on the page.
Coin edges count against the coinage from command `01` and START spends
credits until credit mode is re-entered; see §8.

54XX HLE: high nibble 1, 2, or 5 plays noise for about 1/8 second. 3 and
4 expect 4 parameter bytes, 6 expects 5, 7 sets volume. The noise drives
the 54XX_0 DAC, so it goes through the real filter network. That fixed
burst is still a labelled stand-in for the 54XX program. A real 54XX
image drives all three DACs (§8). The 06XX falling edge asserts /IO on the selected MCU
(MB8843 external IRQ) and NMIs the main CPU; the first read-mode edge
skips the NMI so the mask program can drive the data bus. The rising
edge clears /IO. A data write only latches the byte. The fallback is
what CI tests.

## 6. High score

Derived by disassembling the Midway `galagamw` main program ROM
(`3200a`-`3500d`, CPU 1, $0000-$3FFF; a local dump, not in the repo).

- Init: POST at `$0310` (after the RAM tests and the `$83C0` blank fill)
  does `LD DE,$8A20`, five times `LDIR` of 6 bytes from `$02B9`
  (`00 00 00 00 02 24`: 20000 as digits, least significant first, then a
  `$24` blank). It then builds five 3-byte names from the character codes at
  `$02BF` (`$17 $0A $16 $0C $18`) as `c,$2A,c` (`LD A,$2A` at `$0324`).
  That is the 30 + 15 = `0x2D` bytes at `$8A20`. `CALL $00D6` then copies
  the first 5 bytes at `$02B9` to `$83ED` (the HI-SCORE strip) and draws the
  label. The sixth strip byte `$83F2` is `$24` from the `LD A,$24` fill of
  `$83C0-$83FF` at `$02FD`.
- Layout: five 6-digit scores at `$8A20 + 6n`, best first, then five
  3-character names at `$8A3E + 3n`. Digits are stored least significant
  first with the blank last, so a compare walks down from the last byte.
- Insert: `$3000` (game over). `$8A00` = pointer to the player's score
  strip (`$83FD` P1, `$83E8` P2, by `$9840`). `$31F7` compares 6 digits from
  the top against table entry ends `$8A3D`, `$8A37`, `$8A31`, `$8A2B`,
  `$8A25`, fixes the rank in `$8A11`, shifts entries down with `LDDR`
  (`$31B4`), then copies the score in and runs name entry. `$8A00-$8A11`
  are entry scratch and are not saved.
- On-screen top score: `$83ED-$83F2`, six screen characters. `$077D` copies
  the player's score over it as soon as it exceeds it, so it changes during
  play while the table changes only at game over. Both are saved. Nothing
  copies table entry 0 back to the strip after init.
- Saved: `$8A20` x `0x2D` (table and names) and `$83ED` x 6 (strip). That is
  everything the game keeps across attract cycles.
- Ready and factory: the app builds the image init leaves (table, names,
  strip) from the loaded ROM's `$02B9`/`$02BF` bytes. Restore waits until RAM
  equals it, so POST junk and the pre-init zeros never pass. A save is not
  written while RAM equals it.

IndexedDB `hiscores` in `retroweb-galaga`, keyed by program CRC. Reset HIGH
SCORE is the labelled clear and is disabled on the self-test.

## 7. Front end

Cabinet chrome follows the Galaxian page: coin door (left slot coin 1,
right slot coin 2), 1P/2P start, DIP panel in `localStorage`, shared theme
stack, mute, footer. Gamepad stick, A, Start, and Select use the same
bits as the keys. Lamps stay out. The coin counters are modelled (§8)
but have no on-page meter.

## 8. Parity fixes (2026-10-09)

Items GA1-GA5, B1-B3 and X4 from `shared/cpu/Z80_ARCADE_PARITY.md`.

- **Raster video (B2) and the 05XX clock (GA1).** *Fact:* the 05XX
  clocks its RNG off the pixel clock. *Why:* the old renderer advanced it
  inside `render()`, once per `requestAnimationFrame`, so a 120 Hz monitor
  scrolled the stars twice as fast and a hidden tab froze them. *What:*
  the main Z80's `Bus::tick` drives `Video::advance`. Each HBLANK loads
  the next line's sprite and star buffers: 256 RNG clocks on each of
  the 256 field lines, the scroll trim on the last. Controls are sampled
  at line 0. `render()` only copies the frame.
  `Video.StarfieldAdvancesWithTheBeamNotRender`.
- **Sprite transparency (GA2).** LUT entry 0x0F, not pen 0.
  `Video.SpritePenIsClearOnlyWhenItsLutEntryIs0F`.
- **Flip screen.** Found on the way, checked against the real ROM's
  upside-down POST screen. Flip inverts the timing counters, so the
  counter picks the tile's byte half, and selects the second character
  set (`code | 0x80`), whose nibbles are stored reversed. The shifter
  outputs each nibble in screen order, so the two together give a full
  mirror. The old code mirrored tile positions only, and didn't mask the
  code to 7 bits. `Video.FlipUsesSecondCharSetAndInvertedCounters`.
- **51XX HLE coinage and credits (GA3).** *Fact:* the 51XX counts coins
  against the coinage the game sends with command `01`; zero coins means
  free play; START spends 1 or 2 credits and starts a game. *Why:* the HLE
  gave a credit per coin and never spent one, so the default path could
  not start a game. *What:* checked against the genuine 51XX running in
  this core: three coins then START gives CREDIT 2 and PLAYER 1 on both.
  `Mcu51Hle.*`.
- **54XX DAC network (GA4).** *Fact:* the 54XX drives three 4-bit R
  ladders (O low/high nibble, R1). Each goes through its own MFB
  band-pass into an op-amp summer, then a 0.1 µF coupling cap. *Why:* the
  LLE path mixed the raw O latch, and that latch also held the CPU's
  command bytes. *What:* separate command latch for K/R0; the network at
  96 kHz. Midway schematic M051-00808-C000 (Galaga CPU PCB A084-91404-C508,
  10-12-81) shows the parts at 6M and 5P. Each ladder is 4.7k, 10k, 22k,
  47k. The three filters are 150k/22k/470k/0.01µF, 47k/10k/150k/0.01µF,
  and 100k/22k/220k/0.001µF, mixed through 10k, 33k, and 33k into the
  3.3k summer. (Superseded by §9: the third filter is not fitted.) The + input sits on 3.3k from +5V and 2.2k to ground.
  The 0.1µF into the MB3730 is fed from the 10k volume pot, not from a
  100k. The 100k in the coupling time constant, the 40800 summer scale,
  and the two 0.9 mix factors are not on that sheet. Fujitsu MB3730A
  (1988 Linear Products Data Book) lists 55 dB typical gain and 70 kΩ
  typical input resistance, which are not those numbers either. Filters start at
  rest, so power-on is silent.
  `Machine.UserRom54xxPlaysThroughItsDacs` (real 54XX image).
- **MB88 serial clock (GA5).** The 51XX program enables the serial port
  (PIO `$64`) and polls SF. It does not enable the serial interrupt.
  With SI unconnected, S shifts in 0 once per instruction cycle and SF
  sets after four. The shift period is the datasheet's: S1 is 1/6 of the
  clock generator, and two instruction cycles are 12 clocks, so one
  instruction is one shift. `Mb88.InternalSerialClockSetsSfAfterFourShifts`.
- **Held IRQs.** Main and sub vblank IRQs hold /INT until `$6820`/`$6821`
  D0 is written 0 (coinop.org: `$6820`/`$6821` clear IRQ1/IRQ2); the 06XX and
  sound NMIs are edges. `Machine.VblankIrqHoldsUntilMaskCleared`.
- **Timing tests (X4).** `Video.SoundNmiEdgesAtLines64And192`,
  `Machine.Io06NmiPeriodIs64ShiftedByControlBits`,
  `Machine.SubAndSoundCpusRunInLockStepWithMain`.
- **Watchdog keeps RAM (B1)**, `Machine.WatchdogResetKeepsRam`.
- **Coin counters (B3).** The 51XX P port drives them, active low: P3 is
  counter 1, P2 counter 2 (Unsourced). The real 51XX
  image clicks them itself. The HLE clicks the chute's counter once per
  coin it accepts. `Io.Real51xxDrivesCoinMetersActiveLowOnP`,
  `Mcu51Hle.CoinMetersClickOncePerCoin`. The genuine image pulses P3 low
  once for a chute-1 coin, which `Machine.UserRomInsertsCoinWhenLocalDumpPresent`
  checks. That test used to drop its coin during POST, when the 51XX
  ignores coins. It now waits 20 s.
- **54XX fallback level.** `Io.HleExplosionPlaysThroughTheDacNetworkAndDecays`
  pins the fixed burst's peak through the DAC network (about 0.18) and
  its decay to silence, so CI covers the network without `54xx.bin`.
- **Play test harness.** `play_test`'s zip loader copied a temp-dir guard
  whose destructor deleted the extracted ROMs, so the real-ROM test always
  skipped. The guard is move-only now.

### Found closing the coverage gaps

- **MB88 instruction timing.** The board ran one MB88 instruction per
  instruction-cycle slot. EN, DIS, CALL, JPL and JPA take two cycles, and
  taking an interrupt costs three more (the two-cycle list is the
  datasheet's, the three extra cycles are Unsourced). `step()` now returns every cycle it burns, and the
  board waits them out. `Io.McuSpendsTwoSlotsOnTwoCycleInstructions`.
- **06XX R/W latch.** At real MCU speed the genuine 51XX stalled at
  startup until the watchdog fired. Our K3 read the R/W bit straight from
  the control register, so the stop write ($10) after a command flipped it
  to "read" while the 51XX was still taking that byte. R/W is now latched
  on the 06XX clock's falling edge and left alone when the divider is 0
  (Unsourced). The clock also restarts on the next 06XX
  tick (64 T-states), not half a period later.
  `Io.ReadWriteLineLatchesOnTheClockEdge`,
  `Io.Mcu51SeesTheLatchedReadWriteLineOnK3`,
  `Io.ClockRestartsOnTheNext06xxTick`. With both fixes the genuine 51XX
  reaches the same CREDIT 2 / PLAYER 1 screens as the HLE. Only the
  starfield phase differs.
- **The 54XX isn't readable.** The bus reads `$FF` for it (Unsourced).
  We echoed its command latch. `Io.Reading54xxGivesTheOpenBus`.
- **MB88 ISA suite.** `mb88_test.cpp` checks every opcode, the timer, the
  serial shift, and interrupt entry and return against §5: datasheet
  values where Table 5 or 6 gives them, the ROM-derived and Unsourced
  choices listed there otherwise.
- Also tested now: the real 54XX's K/R0 command and O/R1 DACs, the HLE
  54XX argument counts and volume, the HLE 51XX credit-mode reply, sprite
  RAM banks 2 and 3, IRQ2 masking, and the `$6823` reset line.

## 9. Sound network from the parts

Re-derived from the CPU PCB sheet (page 7-9) and its designation list.

- Fitted: O0-O3 ladder R46-R43 (47K/22K/10K/4.7K) into R42 150K, R41 22K
  to ground, R40 470K, C27/C26 0.01 µF, mixed through R37 10K. O4-O7
  ladder R32-R29 into R33 47K, R34 10K, R35 150K, C29/C28 0.01 µF, mixed
  through R36 33K. The list marks R21-R28, C30-C42 and R49/R50 not used,
  so the R4-R7 ladder and its filter are absent and EXDATA is open. The
  R1 latch is kept (`dac54(2)`) but reaches nothing.
- Pin map from the MB8844 28-pin assignment (MB8840 datasheet Fig. 1):
  O0-O7 on 4-11, R4-R7 on 17-20.
- Each filter is a multiple-feedback band-pass, + input at the R38/R39
  bias (2.0 V), R2 to ground. With R1 = ladder Thevenin + series R, C1
  from node A to the output and C2 from node A to the - input,
  H(s) = -(s C2 R3/R1) / (s² C1 C2 R3 + s (C1+C2) + 1/R1 + 1/R2), output
  DC at bias. The code holds the two capacitor voltages as states,
  measured from rest, x' = A x + B u, and steps them with the trapezoidal
  rule at 96 kHz. f0 is 167 Hz (Q 2.5) and 450 Hz.
  `Io.LowNibbleBandPassRingsNear164Hz` checks the first one's ringing.
- Summer 5P (R20 3.3K) takes both filters and the WSG through R19 10K.
  WSG source: wave ladder R15-R18 (470/1K/2.2K/4.7K) plus the 4066
  volume legs R10-R13 at full volume, C43 2.2 nF on its node (one pole,
  about 20 kHz). The shared WSG chip folds volume in linearly, so the
  WSG level is an approximation of the real 4066 network.
- Output: SOUND through R51 10K onto VR1 1K, C18 0.1 µF into the MB3730
  with C19 0.01 µF to ground and 70K typical input resistance. That is a
  22 Hz high-pass and a 19 kHz low-pass, stepped the same way at the host
  rate. VR1 is taken at full and the amp's own gain is not modelled; the
  host full scale is the summer's 2 V swing below bias.
- Op-amp outputs clamp to 0 V and V+ − 1.5 V (LM324 output swing).
- Unsourced: the 54XX O pins swing the full 5 V rail. The datasheet
  only guarantees VOH ≥ 2.4 V at −200 µA.
- The HLE explosion peaks near 0.09 now (was 0.18).
  `Io.Only54xxPortOLaddersAreFitted`, `Io.SilentBoardIsExactlyZero`.

## 10. 06XX timer and the read delay

The schematic gives 2L's clock as 64H (pin 17), SEL on AB8 (pin 15),
NMI out on pin 19, and its device-side lines to the 51XX and 54XX
(54XX /IRQ on pin 23). The control layout and divider period are still
Unsourced; no non-code description of the 06XX turned up.

The read delay comes from the two ROMs (my own disassembly):

- `51xx.bin` `$002`: every external IRQ reads K and, with K3 (R/W) high,
  lands at `$24F`. That routine puts nibbles `$24`-`$2B` on O (low, then
  high) at Y, steps Y by two, wraps it from `$C` to `$4`, and saves it
  in nibble 6. Only a write IRQ resets it (`$022`-`$024`, Y = 4). So in
  credit mode the reply is a 4-byte ring (credits, two control bytes,
  and a fourth the game never reads), advanced once per select.
- Main ROM `$0299` (IRQ): `HL'=$7000`, `DE'=$99B5`, `BC'=3`, `$7100 = $71`.
  NMI `$0066`: one `LDI` per NMI; after the third it writes `$10`.
  Three NMIs read three bytes and stop the clock.
- For credits to land in `$99B5` every frame, each read must give the
  51XX four selects and the main three NMIs, and NMI k must read the
  byte select k produced. The 51XX takes about 21 instruction cycles
  (~250 T-states) from select to its second O write, so the NMI on the
  same edge as select k is too early. NMI k must follow select k+1 and
  land before the 51XX answers it. With edges only every half period,
  that is the select edge one period later: the first select of a read
  has no NMI. Moving the NMI to the opposite edge instead gives three
  selects per read and the ring drifts a byte per frame.
- Write mode keeps NMI on the first select. The 51XX path doesn't
  distinguish the two (a write resets the ring), so this is Unsourced
  beyond the 54XX traffic working.

`Io.ReadGivesTheDeviceOneMoreSelectThanNmis`;
`Machine.UserRom51xxCreditsReachTheMainCpu` pins it with a local dump.
