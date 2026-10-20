# Battlezone Arcade — review notes

Atari Battlezone (1980) board emulator under `retroweb/battlezone/`.
Fidelity rules follow `CLAUDE.md` and `.claude/arcade.md`.

## 1. Sources

- Atari DP-156 schematic package, 2nd printing (sheet 2A memory map,
  clocks, NMI, watchdog; sheet 2B switch inputs, video output, window and
  intensity circuits; sheet 3A vector generator state machine, stack,
  data shifter, vector timer; sheet 3B Math Box, output latch, audio).
  Scans from jmargolin.com and the archive.org arcade manual collection.
- Atari Battlezone operator's manual (option switch tables fig. 7 and 8,
  self test, R11 adjustment).
- Atari's own Battlezone source and documentation, as published in the
  `historicalsource/battlezone` repository: `BZONE.MAC` (game, equates,
  NMI, windows, sound latch, ALLPOT use), `BZSTST.MAC` (PWRON, factory
  high-score table, self test), `BZSADG.MAC` (AVG diagnostic lists),
  `COIN65.MAC` (coin switches and option bits), `BZONE.MAP` (RAM
  addresses), `BZONE.DOC` (part numbers, PCB locations), `STATE2.MAC`
  (source of the 036408-01 AVG state PROM), `VGMC.MAC` and `VGUT.MAC`
  (AVG instruction encodings, intensity rules), `MBUDOC.DOC` (Math Box
  architecture, microword fields, timing table), `MBUCOD.V05` (the
  microcode source and its `$OUT` PROM layout macro), `MBDIAG.MAC`
  (6502-side Math Box diagnostic). Read for behaviour only. Nothing from
  those files is in this repo.
- Jed Margolin, "The Secret Life of Vector Generators" (jmargolin.com):
  binary normalization, the window sample-and-holds, linear vs binary
  scale, and "Vector Generator Instructions" (`vgcmd.pdf`).
- AMD Am2901 data sheet (source/function/destination tables, P/G carries).
- Atari POKEY data sheet C012294 (`shared/atari/pokey.*`).
- RCA CD4066B, TI LM324, NE555 data sheets (typical levels).

## 2. Board

- 6502 at 12.096 MHz / 8. A15 is not decoded, so `$7FFA` vectors serve
  `$FFFA` (sheet 2A memory map).
- NMI = 3 kHz (12.096 MHz / 4096) / 12 = 246.09 Hz: L4 LS161 loads 4 and
  its ET is /TEST, so the self-test switch on IN0 D4 stops it (sheet 2A,
  `BZSTST.MAC` PWRON).
- IN0 `$0800` through R11 LS244 with 470 Ω pull-ups: D0 right coin, D1
  middle coin, D2 left coin, D3 slam, D4 self-test, D5 diagnostic step,
  all low-true; D6 AVG HALT, D7 3KHZ (sheet 2B). The page's two-slot door
  wires the left slot to the middle mech, as the option sheet's "Center
  (or Left)" says.
- DSW0 `$0A00` (M10) and DSW1 `$0C00` (P10) through N11/P11 LS244s, a
  closed switch reads 0. Factory values are the manual's suggested
  settings: `0x15` (3 tanks, missile at 10000, bonus 15k and 100k,
  English, fig. 7) and `0x63` (2 coins per play, both mechs x1, $1.00 = 3
  plays, fig. 8).
- Treads, fire and start read through POKEY ALLPOT `$1828`, active high
  (`BZONE.MAC`). Only START `$20` is read by the game (`START=20`), so the
  panel has one start button.
- `$1840` M2 LS273 output latch (sheet 3B): D7 MOTOR EN, D6 start LED,
  D5 SOUND ENABLE, D4 ENGINE REV EN, D3 SHELL L/S, D2 SHELL EN, D1
  EXPLO L/S, D0 EXPLO EN. The page lights the START button from D6.
- Watchdog `$1400`: M4 LS393 QD reaches 128 counts of 3 kHz and clocks L10
  into reset (about 43 ms), the same circuit as Asteroids (sheet 2A).

## 3. AVG

`retroweb/shared/atari/avg.*`, from sheet 3A and the Atari source.

- State machine: `kNext[op][state]` is the K7 PROM table written out from
  `STATE2.MAC` (don't-cares as 0); it matches the 036408-01 dump byte for
  byte. L8 gates the state clock with VGCK, 3MHZ and 6MHZ, one state per 8
  master clocks. While the vector timer runs (GO) or HALT is latched, A7
  holds the PROM in state 0.
- J7 LS42 decodes states 8-F into LATCH0-3 and STROBE0-3. LATCH1 takes
  the odd byte (opcode, Y12-8) and clears the X and low Y shifters;
  LATCH0, LATCH3, LATCH2 take Y7-0, Z and X12-8, X7-0. SVEC reaches
  LATCH3 straight from LATCH1, so its low byte lands in the Z/X high
  latch. The byte select toggles on every latch and the word counter
  advances after each even byte.
- Stack: H6/F6/E6 LS670s, four 12-bit words, J6 pointer counting up for
  JSRL and down for RTSL (D/U = OP1). STROBE0 writes, STROBE1 moves the
  pointer, STROBE2 loads H5/F5/E5 from the instruction or the stack.
  DISRST loads J6 with 0; VGGO clears the program counter.
- M9 latches OP0 as HALT on every STROBE3. DISRST (reset or VGRST)
  presets it and clears F8 (STAT intensity), F9 (binary scale) and the
  linear scale latch.
- Normalization: the LS194 shifters move X and Y left until either bit 12
  differs from bit 11, one shift per 12 MHz clock, while the vector timer
  shifts right. SCALE then shifts it once per binary scale step. The DACs
  take DV bits 12-3 (A5 drives only QA).
- Vector timer: four LS161s at 12 MHz, 2^15 counts for VCTR. SVEC holds
  N6/M6 clear, so it counts 2^8; with its 5-bit fields at bits 12-8 that
  makes SVEC n equal to VCTR 2n, as the `VGMC.MAC` macro assumes when it
  picks the short form.
- Linear scale scales the DAC reference: lengths shrink by (256 - L) / 256
  without changing beam time (`VGMC.MAC`, Margolin).
- CNTR (`$8040`) discharges the integrators; its wait comes from
  normalizing the low byte through the same timer.
- Intensity: H8 LS399 picks the STAT level (F8) when the vector's Z is 1,
  else Z drives the top three of the four intensity resistors (sheet 2B,
  `VGUT.MAC` "Z=1 uses the last Z").
- Window: STAT with D10 = 0 clocks H10 from D9, closing the HST or LST
  sample-and-hold on the current beam position until the next LATCH1, and
  J10 latches D8 (IN). B7 XORs the comparators with IN, so IN = 1 lights
  only outside the window. The holds keep their value across GO.
- Picture: `BZSADG.MAC` draws its corner diagonals with SCAL 1 VCTR
  512,384, so full size spans 512 x 384 vector units. The page stretches
  that to the 4:3 tube.

## 4. Math Box

`retroweb/shared/atari/mathbox.*`.

- Four Am2901 slices as one 16-bit ALU with a rippled carry, separate
  I2 for the high and low byte pairs, D and Y tied to the host bus with
  D7 feeding D15, the N xor V sign mux for ASR and JPL, CADD inverting I1
  after a 0 falls out of Q, LDAB jump latch, STALL (all `MBUDOC.DOC`).
- Microword bit order: M1 holds bits 23-20 and F1 bits 3-0. `MBUCOD.V05`'s
  `$OUT` macro puts word bits 23-16 on page `$6800`. Loading the 036175
  (M1) to 036180 (F1) dumps in that order runs the game's 3D view
  (`Play.*`).
- Clock: B4 resamples E3MHZ on E6MHZ, 3.024 MHz, two microcycles per CPU
  cycle (sheet 3B). `$1800` D7 reads 1 while the sequencer runs, as
  `MBUDOC.DOC` and `BZONE.MAC`'s `WAIT` loop expect. The longest documented
  routine (131 + 5N, N = 16) is about 70 CPU cycles, inside `MBDIAG.MAC`'s
  99-cycle timeout.
- Documentation gaps, chosen and labelled:
  - Carry and overflow after EXOR/EXNOR are left at 0 (the data sheet's
    expressions aren't in `MBUDOC.DOC`).
  - Q0 latch after a non-shift destination reads 1 (an undriven TTL line
    floats high). Left shifts force it low, per `MBUDOC.DOC`.
  - RAMU shifts a 1 into RAM0 ("garbage from floating input").
  - The D bus holds the written byte for the whole routine. The microcode
    only uses D in a routine's first instruction.
  - A write starts the sequencer on the next CPU cycle boundary after the
    instruction that made it, not mid-instruction.

## 5. Sound

`battlezone/sound.*` is a component-level model of sheet 3B, Audio Output:

- Noise: H5 then H4 LS164 clocked at E12KHZ / 2, fed by the XNOR of H4
  QG and H5 QD and cleared while SOUND ENABLE is low. The shell gate is a
  J5 toggle on H4 QH; the explosion gate toggles on J4's NAND of stages
  11-14, so explosion noise runs slower.
- Shell and explosion: each 4066 charges its hold cap (C9 4.7 µF, C14
  10 µF) while enabled; the noise gate dumps it through the divider into
  an LM324 stage whose L/S switch raises the gain (R32/C21, R33/C22).
- Engine: MOTOR EN and ENGINE REV EN steer R7/R6/R5 and R8 onto C13 through
  CR1 and R9; K5 buffers it onto the F3 555 control pin (R10, R11, C11).
  F4 loads 4 and F5 loads 6 (/12 and /10), and their QD and RCO reach
  C29 through 33K.
- Mixer: R25, R28, R26 and R27 (POKEY) into K5, gain 11, on +22 V; L4
  4066 passes it only with SOUND ENABLE; AUD- sits at +5 V through
  C20/R24/R23. The output is AUD+ minus AUD-.
- POKEY's AUD pin is open drain into R36 10K to +5 V and C25 0.015 µF;
  `shared/atari/pokey.*` models the pull-down devices from the data
  sheet's volume levels.

## 6. Self-test ROM

`roms/hwtest/gen_hwtest.py` builds an original program, vector ROM and
Math Box microcode under Atari's chip names:

- Signature `BZN1` at `$0000`.
- Its own microcode: byte loads that keep the other half (split I2),
  an unrolled CADD/RAMQD multiply with the N xor V sign mux, and a signed
  max using LDAB + JPL. The 6502 waits on the busy bit, checks the
  product and the max, and shows MATH BOX OK or FAIL.
- AVG diagnostic: opens a full-screen window, then a border, crosshair,
  three boxes, an intensity ramp and a diagonal cut by a small window.
  Then the help screen in this repo's stroke font, which opens the full
  window again, title and SELF TEST OK under the red overlay.
- Main loop starts the AVG once every six NMIs, after IN0 D6 says the last
  picture is done, as Battlezone does.

## 7. Departures and open items

- High-score RAM `$0300–$033B` persists in IndexedDB for user ROMs (real
  PCBs have no battery). Reset HIGH SCORE clears it.
- The window holds start wide open at power-on; their real power-on
  charge isn't documented. The game sets both corners every frame.
- The overlay edge sits at the game's upper window, Y = 192 at SCAL 1
  (`BZONE.MAC`), a quarter of the way down. The film's exact edge isn't
  in the sources.
- The C7 speed-compensation inputs to the intensity node (R29-R32), the
  P9 unblank delay and bus waits while the CPU touches vector RAM aren't
  modelled.
- R11 sits at mid travel; the manual sets it by ear to a moderate idle.
- 4066 on-resistance, LM324 output limit, LS TTL levels and the cabinet
  amplifier's 10 Hz input coupling are typical values, not board
  measurements.
- The 036408 state PROM in a user's set is ignored, since `kNext` is the
  same table.
- The rev 2 (`bzone`, 036414-02) high-score table slants left and its
  last line can touch BONUS TANK. That's the game: rev 2 steps between
  lines with one relative vector of (-268, -40) after 11 characters of
  24 units (264), so each line starts 4 units further left
  (6502disassembly.com Battlezone rev 2 at `$552F`). Rev 1
  (`bzonea`) centres the beam before every line, as `BZONE.MAC` `DISTBL`
  does, and draws it straight.
- Cocktail `bzonec`, Bradley Trainer and Red Baron aren't wired. Red Baron
  shares the AVG and Math Box and would reuse them.
- Coin counters (`$1000`) are latched but not shown.

## 8. Tests

- `make -C retroweb/battlezone check`: AVG, Math Box, sound board,
  machine, board smoke, and `Play.*` (skips unless `roms/user/` or
  `BZONE_ROM` holds a dump; boots to the factory table, coins up, drives,
  shoots, and keeps the Math Box busy).
- `make -C retroweb/battlezone test-smoke` (board smoke)
- `make -C retroweb/battlezone/web test` / `test-smoke` (Playwright on :9500)
