# Galaga arcade board — design notes

Citation trail for the Midway Galaga (1981) emulator. The write-up used
while coding the map is coinop.org GalagaMap
(https://www.coinop.org/kdl/GameTech/GalagaMap.html). MAME `galaga.cpp`,
`namco06.cpp`, `namco51.cpp`, `namco54.cpp`, and `mb88xx.cpp` are
cross-checks of addresses, clocks, and the 06XX command bytes, not the
behavior source. The Midway schematic is the authority when those disagree.

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

Master crystal 18.432 MHz (schematic; same figure MAME uses).

- Each Z80: 18.432 / 6 = 3.072 MHz. Live page stays at that rate.
- 51XX and 54XX: 1.536 MHz input. One instruction is that clock / 6, so
  one instruction per 12 Z80 T-states. The real core runs only after
  `$6823` releases reset and a 1024-byte image was loaded.
- Raster, same numbers as Pac-Man and confirmed against MAME's screen raw
  parameters: 384×264 total, visible 288×224, about 60.606 Hz,
  50688 T-states per frame. Upright cabinet is rotated, canvas 224×288.
  No rotation control.

The three Z80s share RAM through the 08XX bus controllers. This core gives
each CPU its own slot at 18.432/6 and advances sub and sound by the same
T-state count as each main instruction, so they do not wait on each other.
MAME's 6000 Hz scheduling quantum is not the hardware model.

## 3. Memory map

Private program ROM `$0000–$3FFF` per CPU. Shared otherwise.

- `$6800–$6807` — DIP read. The address offset selects the switch bit.
  Bit 0 of the byte is physical SWA (MAME `DSWB`); bit 1 is physical SWB
  (MAME `DSWA`). Factory defaults are SWB `0xF7` (2-credit game = 2
  players, easy, demo sounds on, freeze off, rack test off, unused
  switch open, upright) and SWA `0x97` (1 coin / 1 credit, bonus
  20K/70K/every 70K, 3 lives). SWB bit 6 is MAME's unused SWB:7 and
  reads 1 when open. galagamw's sub CPU (`$0ECA`) resets if that bit
  is clear.
  `$6800` with those defaults reads `0x03`.
- `$6800–$681F` — wavetable sound writes, the same 32 nibbles as Pac-Man,
  clocked at 3.072 MHz / 32 = 96 kHz. The generator lives in
  `retroweb/shared/namco/` so the silicon is not forked. Galaga leaves
  the chip enabled; no separate enable bit was found.
- `$6820–$6827` — 74LS259. Bit 0 IRQ1 enable, bit 1 IRQ2 enable, bit 2
  sound-NMI disable (write 1 turns the NMI off; the enable is active-low
  NMION), bit 3 releases sub CPU + sound CPU + 51XX + 54XX. Power-on
  holds that bit at 0, so sub and sound stay in reset until the main
  program writes 1.
- `$6830` — watchdog reset. 8 vblanks.
- `$7000–$70FF` / `$7100` — 06XX data and control. Control bits 0–3 are
  chip selects (bit 0 = 51XX, bit 3 = 54XX), bit 4 is read/!write, bits
  7:5 are the clock divider. A non-zero divider arms an NMI to the main
  CPU. Period is `(64 << shift)` Z80 cycles. The device clock is
  master/6/64. Startup traffic cross-checked against the namco06 header:
  `10` + `FF`, then `71` (read 3) or `A1` (write 4) or `A8` (write 12),
  each closed with `10`.
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
greyrogue `galaga.vhd` `bgtile_addr` is the source; MAME `tilemap_scan`
is the cross-check. Pen 0 is transparent. 2bpp nibble packing matches
the Namco character layout (high nibble plane 0, low nibble plane 1).
Sprites: 64 × 2 bytes. Each 16×16 picture is four columns at bytes
0, 8, 16 and 24. Attribute bit
0 flip H, bit 1 flip V, bit 2 2×H, bit 3 2×V (greyrogue `galaga.vhd`
`spdata` is the cross-check). X is 10 bits: the position byte plus two
bits in the following register, and register 0 is 40 pixels left of the
visible origin. Y counts down from its register. A 2× sprite is the next
three codes. MAME `video/galaga.cpp` `draw_sprites` is the cross-check.

Stars are the Namco 05XX: a 16-bit Fibonacci LFSR (taps 16/13/11/6)
that paints a hit into the centre 256 pixels when bits match
`FA14`/`7800`. Speed is latches 0–2, blink sets are 3–4 (with set B
forced into {2,3}), enable is latch 5. Hildinger's RE notes (in MAME
`starfield_05xx.cpp`) are the source. Draw order is stars, then sprites,
then tiles, so the score strips stay on top. Character colours OR `0x10`
into the PROM index. Palette resistor weights are the Pac-Man
82S123-style weights; confirm them against the Midway schematic before
treating a color as exact.

## 5. 51XX and 54XX

51XX is a Fujitsu MB8843, 54XX an MB8844. Both: 10-bit PC (1K ROM), 6-bit
data (64 nibbles). The core is `retroweb/galaga/mb88.cpp`. Opcode behavior
is cross-checked against MAME `mb88xx.cpp` (Ernesto Corvi). The ST flag
is inverted versus a normal Z flag. External IRQ vectors to `$02`, the
timer to `$04`. The timer prescale of 32 is included (MAME marks it a
guess and uses it to match PCB recordings). The serial prescaler is not
implemented. Stack is 4×16-bit. `st` resets to 1 so a conditional jump is
taken after reset.

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
Coin edges increment credits only while the HLE is in credit mode.

54XX HLE: high nibble 1, 2, or 5 plays noise for about 1/8 second. 3 and
4 expect 4 parameter bytes, 6 expects 5, 7 sets volume. The noise is an
LFSR mixed onto the wavetable samples. A real 54XX image mixes `o_output`
as a DAC instead. The 06XX falling edge asserts /IO on the selected MCU
(MB8843 external IRQ) and NMIs the main CPU; the first read-mode edge
skips the NMI so the mask program can drive the data bus. The rising
edge clears /IO. A data write only latches the byte. The fallback is
what CI tests.

## 6. High score

MAME `plugins/hiscore/hiscore.dat`, `galagamw` in the `galaga` group:
`$8A20` for `0x2D` bytes and `$83ED` for 6 bytes. IndexedDB `hiscores` in
`retroweb-galaga`, keyed by program CRC. Restore after the main IRQ enable
has been up for 60 frames. Both all-zero and the factory `00 00 00 00 02 24`
prefix (" 20000") count as the reset table, so a save is not written over
POST and a POST table is not stored as a player score. Reset HIGH SCORE
is the labelled clear and is disabled on the self-test. The real factory
bytes should be recorded from a local ROM's POST when one is available;
the prefix above is the published reset table, not a trace from this tree.

## 7. Front end

Cabinet chrome follows the Galaxian page: coin door (left slot coin 1,
right slot coin 2), 1P/2P start, DIP panel in `localStorage`, shared theme
stack, mute, footer. Gamepad stick, A, Start, and Select use the same
bits as the keys. Coin counters and lamps stay out.
