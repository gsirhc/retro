# Space Invaders arcade board: design notes

Citation trail for the Midway Space Invaders (1978) emulator, PCB #739.
Primary source is MAME `src/mame/midw8080/mw8080bw.cpp`, `mw8080bw.h`,
`mw8080bw_v.cpp`, `mw8080bw_a.cpp` and `src/devices/machine/mb14241.cpp` at
commit `d404316732fb5970cc0170aefc69ef1a85e14fce`, whose header comments are
taken from the Midway schematics. The game's RAM use and code paths are
checked against the Computer Archeology disassembly
(computerarcheology.com/Arcade/SpaceInvaders/).

## 0. Status

Single 8080, 1-bit bitmap, MB14241 shifter, and discrete analog + SN76477
sound, covered by GoogleTest (`make check`) and a generated self-test ROM.
The front end ships the self-test. A real Midway `invaders` set is opt-in and
browser-local. Hub card, marquee tile and CI jobs (`invaders-test` /
`invaders-web-test`, both after `i8080-test`) are wired in.

## 1. Scope

MAME parent `invaders` only: four 2K ROMs at `$0000`/`$0800`/`$1000`/`$1800`
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
  H=0 (MAME header, "Vertical sync chain").

## 3. Board split

The board shared with Taito's Lunar Rescue (CPU, memory, I/O decode, MB14241,
interrupts, beam, watchdog) is `retroweb/shared/mw8080/board.{h,cpp}`, the
sound board `retroweb/shared/mw8080/sound.{h,cpp}` with `SoundBoard::Midway`.
`machine.{h,cpp}` here adds the idle inputs, the audio stream and the B&W
render. Tests for the shared parts sit in `shared/mw8080/tests/`.

## 3a. CPU

`retroweb/shared/cpu/cpu_i8080.{h,cpp}`, the Altair's core, moved there so
both machines run one 8080. ISA coverage is the `I8080*` GoogleTest suites
plus 8080PRE, TST8080, CPUTEST and 8080EXM through `cpm/cpm_host`
(`make -C retroweb/shared/cpu i8080diag`).

## 4. Memory map (MAME `main_map`)

- A15 ignored (`global_mask(0x7fff)`).
- `$0000–$1FFF` ROM, writes dropped.
- `$2000–$3FFF` RAM, mirrored at `$6000–$7FFF`. `$2400–$3FFF` is the bitmap.
- `$4000–$5FFF` empty ROM socket, reads 0 as in MAME's zero-filled region.

## 5. I/O (MAME `invaders_state::io_map`, A0–A2 decoded)

Reads: 0 IN0, 1 IN1, 2 IN2, 3 shifter result, mirrored at 4–7.
Writes: 2 shift count, 3 sound 1, 4 shift data, 5 sound 2, 6 watchdog.

Inputs, upright PCB (MAME `INPUT_PORTS_START( invaders )`):

| Port | Bits |
|---|---|
| IN0 | 0 SW8, 1–2 SW6/SW7, 3 pulled high, 4–6 P1 fire/left/right, 7 SW5 |
| IN1 | 0 COIN **active-low**, 1 START2, 2 START1, 3 pulled high, 4–6 P1 fire/left/right |
| IN2 | 0–1 lives (3–6), 2 TILT n/c, 3 extra base at 1000 (0 = 1500), 4–6 P1 controls, 7 coin info off |

The disassembly's ISR at `$0020` reads `IN 01; RRCA; JC` as "coin
deposited", so with MAME's active-low COIN a credit registers when the switch
is pressed. SW5–SW8 are unused by this ROM and not on the DIP panel.

## 6. MB14241

`count = ~data & 7`, `data = data >> 8 | new << 7`, result `data >> count`
(MAME `mb14241.cpp`). Equivalent to the usual description: a 16-bit register
with the newest byte high, read as `reg >> (8 - n)`.

## 7. Interrupts

E3 is a D flip-flop clocked by `(!(64V | !128V) | VBLANK)`, so it fires at
V = 0x80 (line 96) and V = 0xDA with VBLANK (line 224). It latches the 8080's
INTE pin, which is what MAME's `interrupt_trigger` does (assert if
`m_int_enable`, else clear). INTA clears it. The vector is sampled at INTA as
`0xC7 | 64V << 4 | !64V << 3`: RST 1 (`$CF`) at line 96, RST 2 (`$D7`) at 224.
An acknowledge that slips past line 160 reads RST 2, as on the board.
`Board::run_cycles` samples INT after each instruction, once the beam has
caught up.

## 8. Video

- The beam is emulated. Each VRAM byte loads the shift register at pixel
  `8k+4` of its line (MAME `screen_update_invaders`), at 2.5 pixels per
  T-state, so a write lands on screen this frame only if the beam has not
  passed it. Granularity is one instruction.
- The frame is latched at line 224 and shown upright (MAME ROT270): 224×260,
  the bottom four rows are the shift register flushing zeros after HBLANK
  (`MW8080BW_HPIXCOUNT = 256 + 4`).
- The tube is black and white. Color comes from the Midway upright's
  cellophane strips, multiplied over the canvas in `app.js` with MAME
  `invaders.lay`'s rectangles: red y 32–62, green y 184–240, and green y
  240–260 for x 16–134. The moon backdrop art is copyrighted and not drawn.

## 9. Watchdog

255 frames without OUT 6 (MAME `WATCHDOG_TIMER … 255 * MW8080BW_60HZ`). Only
the CPU is reset. RAM, the beam and the sound latches carry on.

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

Assumptions:

- R106 at mid travel (25K).
- R78 reads 200K or 280K on the scan; 200K is used.
- The bonus tone is the sheet's "480 HZ" label as an ideal square.
- SN76477 VCO law f = f_min (2.5 + V0)/(V + V0), V0 = 2.5/9: the data sheet
  gives only the end points (f_min at 2.5 V, 10× at 0 V).
- SN76477 pins 8 and 23 open: attack and one-shot caps treated as zero.
- 7411 outputs at typical TTL levels (3.4 V / 0.2 V); diode drops 0.6 V.
- The noise clock uses the textbook two-gate CMOS astable period.
- By the data sheet's equation 8 the UFO's 0.23 V pulses into 47K come out
  about 30 dB below the fleet. That level comes straight from R123/R124.

## 11. HIGH SCORE

`$20F4–$20F5`, BCD, low byte first (disassembly `HiScorL`/`HiScorM`).
`init` at `$18D4` copies `$1B00–$1BFF` over `$2000–$20FF` and draws the
status line at `$18DC`, so the restore waits 30 frames for that copy to
leave zeros. The game redraws HI-SCORE only at coin-up (`$07C8`) and game
over (`$1695`), so a restored score is live in RAM straight away and on
screen from the next coin-up.

## 12. Self-test ROM (`roms/hwtest/gen_hwtest.py`)

Original 8080 code assembled in Python. Signature `TST1` at `$2000`, an
MB14241 check (result at `$2005`, pass flag at `$2004`), a 16-pixel
crosshatch with the four fleet notes, a full-white field with the saucer
(shows the gels), then the help screen in this repo's own 8×8 font, rotated
for ROT270. RST 1 counts at `$2009`, RST 2 at `$2008`; the main loop feeds the
watchdog.
