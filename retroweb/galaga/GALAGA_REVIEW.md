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
- Fujitsu MB8840/MB8840H series datasheet, January 1987 (Fig. 1 pin
  assignment, p. 1-6, used to read the 51XX/54XX pins on sheet 7-9).
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
  7:5 are the clock divider; `$7100` reads back as written. The layout
  comes from the main ROM's own traffic (§10). The divider period is
  Unsourced. The schematic shows the 06XX as a custom with its pins only.
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

36×28 tile map over the 288×224 visible area. On the video PCB sheet
(galaga3.pdf sheet 7-7, left half) the tile RAM address AB0-AB10 comes
out of the 1L 00XX custom. Its inputs are 128V-8V from the 1N 07XX (00XX
pins 13-9), five more 07XX H outputs (07XX pins 9-5 into 00XX pins 7-3),
4H and 2H (pins 2, 1), 07XX pin 10 through the 3N inverter (pin 8), and
FLIP (pin 27). The custom's internal mapping isn't drawn. `paint()` takes
those as 8H-128H, 8V-128V and 256H: in the playfield (256H high) A9-A5
is the V tile row and A4-A0 the H column; with 256H low the two fields
swap, so the two columns each side are memory rows 0-1 (native right,
the bottom of the upright screen) and 30-31 (native left, the top),
walked by V. That is the layout the game's own text needs: a credit line
stored across row 1 reads along the bottom. The counts at the first
visible dot (H `0xF0`, V 16) are Unsourced, placed so 256H spans the 256
playfield dots. FLIP inverts 8H-128H and 1V-128V. A tile pen is clear when its LUT entry ORed with 0x10
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
answers the command stream with those bits inverted on the R ports. Every
rule below comes from my disassembly of `51xx.bin` (addresses are ROM
addresses; `[X][Y]` is the MB8843 RAM nibble at that row and column) and
is checked against the genuine image running in this core by
`Machine.UserRom51xxAndFallbackAnswerTheMainCpuAlike`, which sends the same
commands, coins and START presses to both and compares every byte the main
CPU reads.

- Dispatch. `$003` INK, `$006` JPA page 1 jumps to `$040 + 4K`, so only
  K0-K2 (data bits 0-2) count: `$044` command 01 (`$008`-`$021`), `$048`
  02 (`[0][3]` = 0), `$04C` 03 (`[0][2]` = 1), `$050` 04 (`[0][2]` = 0),
  `$054` 05 (`[0][3]` = 1); 0, 6 and 7 return at `$022`.
- Boot. `$094` sets `[0][3]` = 1, so the chip starts in switch mode and
  ignores coins until command 02.
- Command 01. `$013`-`$01D` takes four arguments, one INK each, into
  `[1][3]`, `[1][2]`, `[1][1]`, `[1][0]`: coins per credit and credits per
  coin for chute A, then B. Any argument of 0 stores `$A` in the credit
  tens digit `[1][A]` (`$016`-`$019`). That digit reads back as `$A0 |
  units` and skips all coin counting (`$0C3`-`$0C9`), which is free play.
  The real chip reports `$A0`, not 99. A zero in the first argument puts
  the digit back after every START (`$1F3`-`$1F6`); a zero in any other
  argument loses it on the first START (credits then read 99).
- Switch mode (`[0][3]` = 1). `$0A4`-`$0BB` fills the ring with `R2|R3<<4`
  (IN1), `R0|R1<<4` (IN0), then `$FF`.
- Credit mode (`[0][3]` = 0 waiting, 2 playing). `$0E7`/`$265` write the
  BCD credits (tens in the high nibble) to the first ring byte on every
  pass. Only modes 1 and 2 refresh the other two bytes, so waiting
  re-reads the last values (`$FF $FF` after boot, the last game's after
  one).
- Coins. `$0CA`-`$0DA` keeps four frames of R3 and fires on a press held
  two frames after two released. Chute A `$187`-`$1A3` counts `[1][D]`
  against `[1][3]`, then adds `[1][2]` with ADC, DAA and the tens carry;
  chute B `$1A4`-`$1C5` uses `[1][1]` and `[1][0]`; R3 bit 2 (`$1C6`-`$1D6`)
  adds one credit with no counter. A tens digit reaching `$A` clamps to 99
  (`$19A`-`$19E`).
- START. `$100`-`$11E`, `$1D7`-`$1F7`: only in waiting mode. START 1 needs
  one credit or more, START 2 needs two (`$10F`-`$116`), START 1 wins a tie.
  The chip subtracts 1 or 2 with SBC and DAS, enters mode 2, and ignores
  START until command 02 puts it back to waiting.
- Playing. `$1F9`-`$237`: the second byte is `R0`, the third `R1`, each
  through the `$280`-`$2BD` table (via `$272`) unless command 03 is set,
  and each topped by two button bits from `R2` bits 0 and 1: bit 0 is the
  edge `~(s3 & s2) | s1 | s0` over the last four frames (`$0E9`-`$0F9`,
  low only on the second pressed frame), bit 1 the current state. START
  and coin bits are not reported.
- Service. R3 bit 3 (`$0C0`-`$0C1`, `$239`-`$24D`) clears the credits and
  sets switch mode.
- Ring. `$24F`-`$264` outputs one byte per select, four bytes with the
  fourth unused (`[2][A]`, `[2][B]` = `$F`, `$08C`-`$093`). The fallback
  indexes by read count instead, so it matches only when the main CPU reads
  three bytes per pass.
- Frame loop. `$17E`-`$186` waits for TL to change, and TC is the
  vblank, so the chip samples its inputs once a frame. The fallback does
  the same for the stick, buttons and the R2 history.

Simplifications, all on the fallback's side: coins and START act on the
first sample, not after two stable frames (the genuine chip ignores a coin
for a frame or two after a command, and a START in the frame it credits a
coin, `$100`-`$103`); the coin counter clicks once per coin where the
genuine chip paces and queues pulses by TL (`$120`-`$172`, `[1][B]`
saturating at 15); the R2 history starts released where the chip's cleared
RAM reads pressed. Cocktail player 2 stays unmapped and is labelled on
the page.

54XX HLE: high nibble 1, 2, or 5 plays noise for about 1/8 second. 3 and
4 expect 4 parameter bytes, 6 expects 5, 7 sets volume.

The counts come from my disassembly of `54xx.bin`. `$004` JPA page 6 sends
K = 3, 4, 6 to `$255`, `$25D`, `$240`. Each sets a pointer in R5 (7, 15, 9),
then loops at `$265`/`$268`: wait for /IRQ low and high, store R0 and K at
the pointer (`$26B`-`$26F`, two nibbles, pointer down by two). `$272`-`$274`
rotates the pointer left and leaves the loop when it equals 14 (pointer 7
or 15). That is 4 pairs from 7, 4 from 15, and 5 from 9 (the first pair of
`$240` is inline at `$24D`-`$253`). `$1C0`, `$1DC`, `$1F9` and `$168`
(K = 1, 2, 5, 7) never test /IRQ and end in RTI, so they take no arguments.
`$168` stores R0 into the volume nibble. The noise drives
the 54XX_0 DAC, so it goes through the real filter network. That fixed
burst is still a labelled stand-in for the 54XX program. A real 54XX
image drives all three DACs (§8). The 06XX model is in §10. The
fallback is what CI tests.

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
  not start a game. *What:* rules and ROM addresses in §5. The genuine
  51XX reports free play as `$A0`, not 99, and in play reports the stick
  and button flags in the second and third bytes (the old pass-through of
  raw IN1 put no fire button where the game looks). The HLE sampled
  inputs live; it now samples once a frame like the chip.
  `Mcu51Hle.*`, `Machine.UserRom51xxAndFallbackAnswerTheMainCpuAlike`.
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
- **06XX timing.** Rebuilt in §10.
- **The 54XX isn't readable.** The bus reads `$FF` for it.
  `Io.Reading54xxGivesTheOpenBus`.
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

What the CPU PCB sheet (galaga3.pdf sheet 7-9, right half; also
`Galaga.pdf` PDF p. 76) shows of 2L 06XX: CPU data DB0-DB7 on pins
20-27, AB8 on pin 15 (data vs control), /I/O on pin 18, 64H on pin 17,
/NMI1 on pin 19. Device side: pins 2-9 are the 8-bit device bus to the
51XX O0-O7 (4H pins 13-20) and the 54XX K0-K3/R0-R3 (6M pins 12-16,
24-27), pin 13 is the 51XX /IRQ (4H pin 4), pin 10 the 54XX /IRQ (6M pin
23), and pin 1 goes to the 51XX K3 (4H pin 41). Pin numbers on the MCUs
read with the MB8840 datasheet Fig. 1.

51XX K0-K2. Read again on the 600 dpi cleaned scan of the same drawing
(jrok.com `galaga_CPU_600_cleaned.pdf`, A084-91404-C508), which is sharp
enough to leave no doubt about what is drawn:

- DB0-DB7 enter 2L on pins 20-27 in order.
- Device pins 2-9 fan out in order: pin 2 to 51XX O7 (4H pin 20) and
  54XX K3 (6M pin 27), down to pin 9 to 51XX O0 (4H pin 13) and 54XX R0
  (6M pin 12). 54XX K0-K3 are pins 5-2, R0-R3 pins 9-6.
- 51XX K2, K1, K0 (4H pins 40, 39, 38) are dotted onto the pin 6, 7 and
  8 lines, the O3, O2 and O1 wires. K3 (pin 41) is pin 1. No wire from
  pin 9 reaches a K pin.

The ROMs fix the bit order on both sides:

- Reads: the 51XX's O0-O7 reach DB0-DB7 unchanged. The $24F ring puts
  BCD credits on O, low nibble first, and main $0299 stores the byte at
  `$99B5` as the credit count.
- 54XX writes: `54xx.bin` `$003` INK, `$004` JPA page 6 dispatches on K
  alone (`$180` + 4K: K = 1-7 are commands, 0 and 8-15 return at
  `$1BC`). Commands 3, 4 and 6 (`$255`, `$25D`, `$240`) then take each
  argument as IN R0 (low nibble) and INK (high nibble). The main ROM's
  `$35A9` list `30 40 00 02 DF 40 30 30 03 DF 10 20` parses as command 3
  plus four arguments, command 4 plus four, then commands 1 and 2, only
  with K = DB4-DB7 and R0 = DB0-DB3. That is the straight-through order
  the drawing shows for pins 2-9.
- 51XX writes: `51xx.bin` `$003` INK, `$006` JPA page 1 dispatches on
  all four K bits (`$040` + 4K). K3 set (read) goes to `$24F`. K = 1 goes
  to `$008`, the only handler that takes arguments (four times TSTI and
  INK, `$00D`-`$01E`). K = 2 (`$048`) and 5 (`$054`) clear and set the
  mode nibble 3, and 3 and 4 set and clear nibble 2. The main ROM sends `05` four
  times in POST (`$35A5`, control `$A1`), `02` three times at `$0738`
  (`$61`), and `01 01 01 01 01 02 03 00` from `$9280` (`$E1`): coinage
  with four arguments, then credit mode. That only parses with K0-K2 =
  DB0-DB2. With the drawn taps (DB1-DB3), `01` would be a no-op, `02`
  would start the coinage routine and swallow `03 00`, and the POST's
  `05` would select credit mode.

Pins 6-9 carry DB3-DB0 on writes (the 54XX needs it), so no 06XX
reordering can give the 51XX taps on pins 8-6 bits 0-2. The drawing's
K0-K2 taps are one line off; the board must have them on pins 9-7. The
code wires K0-K2 to device bus bits 0-2 from the ROMs. A continuity
check on a real CPU board (4H pin 38 to 2L pin 9) would confirm it.

From the main ROM (`3200a`-`3500d`, my own disassembly):

- Control values written: `$10` (reset vector `$0000`, and the NMI
  handler at `$0070` after its last byte), `$71` and `$B1` with HL=$7000
  (reads), `$61`, `$A1`, `$E1` to the 51XX and `$A8` to the 54XX with
  DE=$7000 (writes). So bit 0 is the 51XX, bit 3 the 54XX, bit 4 set is
  read, bits 7:5 zero stop. No transfer selects two chips, reads in
  write mode or writes in read mode.
- `$06F3` and `$37EC` poll `$7100` until it reads `$10`, so the control
  register reads back what was written.
- NMI `$0066` is one `LDI` per NMI; after the last byte (`BC` = 0) it
  writes `$10`. A further NMI would run `LDI` with BC wrapped to `$FFFF`,
  so `$10` must stop the NMIs.

From `51xx.bin` (my own disassembly): the external IRQ at `$002` does
`INK` at `$003` (K3 picks read or write) and `DIS $04` at `$004`, and
only re-enables the external interrupt (`EN $04`) at `$026` on the
write path and `$261` on the read path, just before `RTI`. The datasheet
latches the external request only while it is enabled, so a select that
falls inside that handler is lost and the 4-byte read ring slips. With
selects at the divider's rate that never happens: `$71` gives 512
T-states between selects, longer than the read path.

The model (`io06_tick`, `io06_read`, `io06_write`, `io06_ctrl`):

- 64H gives the 06XX a tick every 64 T-states. Divider d (1-7) taps bit
  d-1 of a free-running count of those ticks, a period of 64 << d.
- A select starts as the tap rises: /IO goes low on each chip whose
  control bit is set, and the main CPU gets an NMI (except the first
  select after a read-mode control write, below). /IO goes high as the
  tap falls, or at the next tick after a zero divider.
- A select never starts part-way through a high phase. If a control
  write lands while the tap is high, the first select waits for the next
  rise. Starting at once gives a runt select a few ticks before the next
  one, inside the 51XX handler, and the genuine 51XX's credits land a
  byte off in `$99B5` (`Machine.UserRom51xxCreditsReachTheMainCpu`).
  `Io.SelectWaitsForTheDividerTapToRise`,
  `Io.NoSelectPartWayThroughAHighPhase`.
- R/W (pin 1, the 51XX K3) is control bit 4 as written.
  `Io.ReadWriteLineIsControlBit4`, `Io.Mcu51SeesTheReadWriteLineOnK3`.
- A data read with bit 4 set and the 51XX selected returns the 51XX's O
  port (or the fallback's reply). The 54XX's bus pins are inputs, so it
  never drives the bus. Anything else reads `$FF`: nothing drives the
  CPU data bus and it floats up on its 2.2K pull-ups (RM19 at J1, sheet
  7-9). `Io.WriteModeReadGivesTheOpenBus`, `Io.Reading54xxGivesTheOpenBus`.
- A data write with bit 4 clear goes onto the device bus for every
  selected chip. With bit 4 set the device owns the bus and the write
  reaches nothing. `Io.ReadModeHostWriteReachesNoDevice`.

Unsourced, chosen as the simplest behaviour the genuine ROMs work with:

- The power-of-two divider, free-running from power-on. The ROMs bound
  it from below only: the select spacing must outlast the 51XX's read
  handler. Checked at 18 offsets of the count: the genuine 51XX and
  54XX boot without a watchdog reset and three coins reach `$99B5`.
- Selects and NMI on the tap's rising edge, /IO released on the falling
  edge. Polarity of the 06XX's own clock isn't visible.
- R/W following the control register at once. The genuine 51XX reads K
  at `$003`, before the main CPU's NMI handler writes `$10`, so it works
  either way; nothing in either ROM needs a latched line.
- `$FF` for reads in write mode or with no driving chip, and writes in
  read mode dropped. Neither ROM does either.
- The control register clears on reset. The schematic shows no reset pin
  on 2L; the main ROM writes `$10` first thing anyway.
- The NMI isn't gated by the MCU reset latch (`$6823`). Sheet 7-9 draws
  no such path from /RESET to 2L.

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
