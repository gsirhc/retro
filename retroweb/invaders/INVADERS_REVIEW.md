# Space Invaders arcade board: design notes

Citation trail for the Midway Space Invaders (1978) emulator, PCB #739.
The sound board is the Midway 739 logic schematic (P.C. A084-90700-B000,
logic schematic M051-00739-A005). The game's RAM use and code paths are
checked against the Computer Archeology disassembly
(computerarcheology.com/Arcade/SpaceInvaders/). Clocks, the memory map,
I/O, the shifter, interrupts and the beam are the model in
`shared/mw8080`. Numbers not read off that schematic or the disassembly
are under Unsourced.

## 0. Status

Single 8080, 1-bit bitmap, shift register, and discrete analog + SN76477
sound, covered by GoogleTest (`make check`) and a generated self-test ROM.
The front end ships the self-test. A real Midway `invaders` set is opt-in and
browser-local. Hub card, marquee tile and CI jobs (`invaders-test` /
`invaders-web-test`, both after `i8080-test`) are wired in.

## 1. Scope

Upright Midway set only: four 2K ROMs at `$0000`/`$0800`/`$1000`/`$1800`
(`9316b-0869_m739h.h1` … `9316b-0854_m739e.e1`, or the older `invaders.h` …
`invaders.e`), or one 8K `program.bin`. Upright PCB only. Taito's `sitv`/`sicv`
and the cocktail PCB are not modelled. On the upright PCB the flip-screen line
(OUT 5 bit 5) is not connected and IN2 carries P1 controls, so there is no
cocktail stick to map.

## 2. Clocks (never sped up on the live page)

- Master 19.968 MHz. 8080 at /10 = **1.9968 MHz**. Pixel clock /4 = 4.992 MHz.
- 320 pixel clocks a line = **128 T-states**. 262 lines = **33,536 T-states a
  frame**, 59.54 Hz.
- The V counter steps when H overflows inside HBLANK, so each line starts at
  H=0.

## 3. Board split

The board shared with Taito's Lunar Rescue (CPU, memory, I/O decode, shifter,
interrupts, beam, watchdog) is `retroweb/shared/mw8080/board.{h,cpp}`, the
sound board `retroweb/shared/mw8080/sound.{h,cpp}` with `SoundBoard::Midway`.
`machine.{h,cpp}` here adds the idle inputs, the audio stream and the B&W
render. Tests for the shared parts sit in `shared/mw8080/tests/`.

## 3a. CPU

`retroweb/shared/cpu/cpu_i8080.{h,cpp}`, the Altair's core, moved there so
both machines run one 8080. ISA coverage is the `I8080*` GoogleTest suites
plus 8080PRE, TST8080, CPUTEST and 8080EXM through `cpm/cpm_host`
(`make -C retroweb/shared/cpu i8080diag`).

## 4. Memory map

- A15 ignored.
- `$0000–$1FFF` ROM, writes dropped.
- `$2000–$3FFF` RAM, mirrored at `$6000–$7FFF`. `$2400–$3FFF` is the bitmap.
- `$4000–$5FFF` empty ROM socket, reads 0.

## 5. I/O (A0–A2 decoded)

Reads: 0 IN0, 1 IN1, 2 IN2, 3 shifter result, mirrored at 4–7.
Writes: 2 shift count, 3 sound 1, 4 shift data, 5 sound 2, 6 watchdog.

The Midway ROM (`invaders.h` at `$0000`, then `.g`, `.f`, `.e`) never
executes `IN 0`. The bits it does test:

| Port | Bits |
|---|---|
| IN1 | 0 coin, credit on the 1→0 edge. 1 start 2, high. 2 start 1, high. 4 fire, high. 5 left. 6 right |
| IN2 | 0–1 bases, the value plus 3. 2 high takes the stack reset. 3 high selects bonus `$10`, low selects `$15`. 7 high skips the coin-info text |

`$0020` is `IN 1` / `RRC` / `JC $0067`. The jump arms `$20EA`. The other
path increments `$20EB` when that flag was set, so a pressed coin switch
reads 0. Start 2 is the second `RRC` at `$085F` (`$20CE` = 1). Start 1 is
`ANI $04` at `$0791`. Fire is `ANI $10` at `$163C`. Left is the third `RLC`
at `$036C` (`DCR $201B`, stop `$30`). Right is the second `RLC` at `$0368`
(`INR $201B`, stop `$D9`). Bases are `$08D1`. The stack reset is `$17CD`.
The bonus threshold is `$093F`. Coin info is `$0BB7`.

`$17C0` returns port 1 when `$2067` bit 0 is set, port 2 when it is clear.
The startup image is `$21`, so the upright controls are port 1. The carry
path at `$0310` stores `$22`, and the same fire, left, and right tests then
read port 2. Bit 3 is set in the idle value. This ROM does not test it, and
it does not read port 0.

## 6. Shifter

Midway 739 logic schematic (M051-00739-B005): 74175 A5 latches D0–D2 on
OUT 2 as EA0–EA2. 74174 C5/D5 and 74175 B5 latch each OUT 4 byte as
DS-8…DS-F and move the previous one down to DS-0…DS-7 minus bit 0, so
the register is 15 bits. Eight 74LS151s pick DS-(k+8-EA) for result bit k.
That is a 16-bit register with the newest byte high, read as `reg >> (8 - n)`.

## 7. Interrupts

E3 is a D flip-flop clocked by `(!(64V | !128V) | VBLANK)`, so it fires at
V = 0x80 (line 96) and V = 0xDA with VBLANK (line 224). It latches the 8080's
INTE pin, so the line asserts only while interrupts are enabled. INTA clears it. The vector is sampled at INTA and 64V
selects it: RST 1 (`$CF`) at line 96, RST 2 (`$D7`) at 224.

Traced on the Taito BPR00003 mother board drawing (archive.org
`space_invaders_manual_schematics_ptm`, sheet 2 at full scan resolution).
The 74174 at 7D latches the 8080 status byte; its Q1 (D0, INTA) feeds both
6D 7402 NORs that drive the A and B selects of the four 74LS153 input
muxes (2A-2D), so INTA selects input 0 (pins 6 and 10). Input 0 of the D0,
D1, D2, D6 and D7 muxes is the common pulled-up line. Input 0 of the D4 mux
(2B pin 6) is 7E QC, the 9316's 64V output, which is the 120 Hz edge
connector line V. Input 0 of the D3 mux (2C pin 10) is the same line
through a NAND with both inputs tied, so `/64V`. The bus at INTA is
`11 0 64V /64V 111`.
An acknowledge that slips past line 160 reads RST 2, as on the board.
`Board::run_cycles` samples INT after each instruction, once the beam has
caught up.

## 8. Video

- The beam is emulated. Each VRAM byte loads the shift register at pixel
  `8k+4` of its line, at 2.5 pixels per
  T-state, so a write lands on screen this frame only if the beam has not
  passed it. Granularity is one instruction.
- The frame is latched at line 224 and shown upright: 224×260. The bottom
  four rows are the shift register flushing zeros after HBLANK.
- The tube is black and white. Color comes from the Midway upright's
  cellophane strips, multiplied over the canvas in `app.js`. The real strip
  measurements are not published, so the edges are an approximation derived
  from where the ROM draws things. Upright row = 255 minus the pixel number
  along the byte (`$1474` shifts H:L right three; H = column + `$20`).
  - Saucer: object L = `$D0` (init `$1B89`), 8 rows, upright y 40–47. Red band
    = object rows with 8 rows of margin: y 32–55. It starts right under the
    score digits (L = `$1C`, y 24–31).
  - Shields: `$0224` draws four 22-column, 2-byte-tall images from `$2806`,
    stride 45 columns, upright y 192–207. Player cannon: L = `$20` (init
    `$1B1A`), y 216–223. Base line: `$01CF`, bit 0 of byte 2, y 239.
    Green band = shield top minus 8 rows to the base line: y 184–239.
  - Bottom strip, y 240–247: reserve count at column 8 (`$2501`), reserve
    cannons from column 24, 16 columns each (`$19E6`), at most 5 on a 6-ship
    DIP. CREDIT label and digits sit at columns 136–207 (`$193E`, `$194A`).
    Green strip = columns 0–111 (count minus 8, five cannons plus 8), y 240 to
    the bottom of the frame. The credit text stays white.
  - Tint is a design choice: pure primaries, (255,0,0) and (0,255,0), so the
    other channels are fully blocked.
- The moon backdrop art is copyrighted and not drawn.

## 9. Watchdog

255 frames without OUT 6. Only the CPU is reset. RAM, the beam and the
sound latches carry on.

## 10. Sound (`shared/mw8080/sound.cpp`)

Built from the Midway 739 logic schematic (P.C. A084-90700-B000, logic
schematic M051-00739-A005; scan `Daughterboard.tif` in classicgaming.cc's
`space-invaders-midway-schematics.zip`) and the part datasheets. Every block
is stepped at 48 kHz with its own equations: LM3900 stages solved as current
mirrors (inputs at one VBE, output 0.09 V to 11.5 V), integrators and
Schmitts switched at the exact threshold crossing inside a step, RC ladders
by implicit Euler.

- OUT 3: 0 UFO (F3 inverts it onto SN76477 pin 9), 1 missile, 2 explosion,
  3 the circuit the sheet labels SAUCER HIT, 4 bonus base, 5 amplifier
  enable (D13/D14).
- OUT 5: 0–3 fleet ladder, 4 the circuit labelled INVADER HIT, 5 flip
  (n/c upright).
- UFO, SN76477 H2: pins 1, 22, 19 high, 25–28 low: VCO swept by the SLF,
  mixer VCO only, one-shot envelope with pins 23/24 open, so the latch set
  by pin 9 falling never times out and pin 9 alone gates the sound. SLF
  0.64/(R120 C35) = 5.3 Hz, a triangle between 0.35 V and 2.5 V. VCO
  minimum 0.64/(R121 C36) = 349 Hz, swept to about 1.5 kHz. Output pulses
  3.4 R124/R123 = 0.23 V into R125 47K.
- Noise: 4006 N5 wired as 17 stages, stage 5 XOR stage 17 back into D2
  (a maximal 131071-bit sequence), NOISE taken at stage 13, clocked by the
  4030 R113/C46 astable at 1/(2 ln3 RC), about 8.1 kHz.
- Missile: M4 one-shot (C12 1 µF, about 0.33 s) charges the M4 integrator
  (R35/R36/C13), which falls at 35 V/s and sets the P4 oscillator's pitch
  through D1/R14; noise is mixed in through C9/R10/R11/R12. P4 stage 3 scales
  the square by that ramp, stage 4 gates it with C14, charged through D3
  while the bit is held and bled through R60. Out through C11 and R28.
- Explosion: K3 one-shot (C24 2.2 µF, about 0.7 s) charges C26 through
  R93/D6; C26 bleeds through R83 (0.68 s). K3-4 subtracts the noise (R79),
  so the output is noise whose level follows C26, then R84/C27/R85/C28/R86.
- Saucer hit (OUT 3 bit 3): K5 sawtooth (C16, about 12 Hz) sweeps the K5
  linear VCO (C17 330 pF, 170 Hz to 2.1 kHz, 4016 J5 as the slope switch).
  K4 one-shot (C18) drives the K4 integrator (R53/R137/C19), K4 stage 3
  subtracts the VCO ramp from that envelope. Out through R59.
- Invader hit (OUT 5 bit 4): M5 triangle (C21, about 6 Hz) sweeps the M5
  VCO (C22 470 pF). The bit charges C23 through the 1K pull-up and R72
  (102 V/s up, 11.5 V/s down). Out through R78.
- Fleet: the set bits pull up through 1K, R126+R127/R128/R129/R130 and a
  diode into the H4 556 discharge pin, R131 75K and C29 0.1 µF. One bit at a
  time gives 61, 53, 50 and 46 Hz. Out through H3, R132/C31/R133/C32, R134.
- Bonus base: the other 556 half (R94, R95, C34, reset by the bit) gates the
  sheet's 480 Hz line in H3, about 7.4 bursts a second. Out through R136.
- Mixer: every source is a current into the L2-3 virtual ground through C44.
  Feedback is R105 100K in parallel with R106 (50K volume) plus C42, bias
  R104 220K. The browser sample is that stage's swing about its 5.7 V bias,
  so ±1.0 is the LM3900 rail.

Sources: the Midway sheet above; TI SN76477 data sheet (July 1978) and
"Complex Sound Generation Using the SN76477N" application guide (both from
sandsoftwaresound.net); TI LM3900 data sheet (SLOS059) and National AN-72;
TI NE555 data sheet; TI CD4006B data sheet.

Not on the sheet:

- R106 at mid travel (25K).
- R78 reads 200K or 280K on the scan; 200K is used.
- The bonus tone is the sheet's "480 HZ" label as an ideal square.
- SN76477 VCO law f = f_min (2.5 + V0)/(V + V0), V0 = 2.5/9: the data sheet
  gives only the end points (f_min at 2.5 V, 10× at 0 V).
- SN76477 pins 8 and 23 open: attack and one-shot caps treated as zero.
- 7411 outputs at 3.4 V / 0.2 V are the typical VOH and VOL in the TI SN5400/SN7400
  data sheet's standard-series table. The SN7411's own data sheet
  could not be fetched, so the family table stands in for it.
- Diode drop 0.6 V (`kVd`) is below the onsemi 1N914/1N4148 data sheet's VF
  of 0.62-0.72 V at 5 mA, for the sub-milliamp currents here. The schematic
  parts are not confirmed to be 1N914s.
- The noise clock uses the textbook two-gate CMOS astable period.
- By the data sheet's equation 8 the UFO's 0.23 V pulses into 47K come out
  about 30 dB below the fleet. That level comes straight from R123/R124.

## Unsourced

- D5 at INTA: 2B pin 10 shows only a stub on the scan. It reads 0 because
  the ISRs sit at `$08`/`$10`.
- Watchdog modulus of 255 frames.
- Empty socket at `$4000` reads 0.
- VRAM byte loaded at pixel `8k+4`, and the four flushed rows.
- Idle inputs `{0x08, 0x09, 0x00}`. IN1 bit 0 high is the ROM (`$0020`
  `IN 1` / `RRC` / `JC`, a pressed coin reads 0). The Midway input sheet is
  not on hand. The Taito BPR00003 sheet 2 muxes wire the connector differently
  (470 ohm pull-ups, IP0-IP7), so it doesn't settle Midway's IN0 value, IN1
  bit 3 or the IN2 DIP defaults. The ROM never reads port 0 and never tests
  IN1 bit 3.

## 11. HIGH SCORE

Derived by disassembling the Midway `invaders` program ROM (`invaders.h`,
`.g`, `.f`, `.e`; a local dump, not the repo).

- Init: reset jumps to `$18D4`, which does `CALL $01E6` (`LXI D,$1B00;
  LXI H,$2000`, then the copy loop at `$1A32` with B = 0, so 256 bytes), then
  `CALL $1956` to draw the score line. The copied page is the game's whole
  variable block, so every variable starts as ROM `$1B00 + n`.
- Table: one entry, two BCD bytes, low byte first, at `$20F4-$20F5`. The
  factory value is ROM `$1BF4-$1BF5`, which is `00 00`. `$20F6-$20F7` is the
  screen pointer for the HI-SCORE field and is not saved.
- Insert: game over reaches `$166D`/`$1671` (from `$02DB`). `$1910` clears the
  player's score-changed byte, `$09CA` returns the score record (`$20F8`
  for P1, `$20FC` for P2: low, high, screen pointer). The code compares the
  high bytes against `$20F5`, then the low bytes against `$20F4`. If the
  score is larger it copies both bytes into `$20F4-$20F5` and calls `$1950`
  to redraw. `$1956` (coin-up at `$07C8`, init) redraws all the score
  fields, so a restored value shows from the next coin-up.
- Saved: `$20F4-$20F5`. The on-screen top score is drawn from it, with no
  second copy.
- Ready and factory: RAM equals ROM `$1BF4-$1BF5` (the app reads the loaded
  program) and at least 2 frames have run. The 256-byte copy is under
  5000 cycles, less than a frame, so the frame floor only keeps the pre-init
  zeros from passing.

## 12. Self-test ROM (`roms/hwtest/gen_hwtest.py`)

Original 8080 code assembled in Python. Signature `TST1` at `$2000`, an
shifter check (result at `$2005`, pass flag at `$2004`), a 16-pixel
crosshatch with the four fleet notes, a full-white field with the saucer
(shows the gels), then the help screen in this repo's own 8×8 font, rotated
for ROT270. RST 1 counts at `$2009`, RST 2 at `$2008`; the main loop feeds the
watchdog.
