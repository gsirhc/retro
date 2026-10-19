# Battlezone Arcade — review notes

Atari Battlezone (1980) board emulator under `retroweb/battlezone/`.
Fidelity rules follow `CLAUDE.md` and `.claude/arcade.md`.

## 1. Sources

- Atari's own Battlezone source and documentation, as published in the
  `historicalsource/battlezone` repository: `BZONE.MAC` (game, equates,
  NMI, sound latch, ALLPOT use), `BZSTST.MAC` (PWRON, factory high-score
  table, self test), `COIN65.MAC` (coin switches and option bits),
  `BZONE.MAP` (RAM addresses), `BZONE.DOC` (part numbers, PCB locations,
  PROM bit masks), `STATE2.MAC` (source of the 036408-01 AVG state PROM),
  `MBUDOC.DOC` (Math Box architecture, microword fields, timing table),
  `MBUCOD.V05` (the microcode source and its `$OUT` PROM layout macro),
  `MBDIAG.MAC` (6502-side Math Box diagnostic). Read for behaviour only.
  Nothing from those files is in this repo.
- MAME `src/mame/atari/bzone.cpp`, `bzone.h`, `bzone_a.cpp`,
  `mathbox.cpp`, `src/devices/video/avgdvg.cpp`, `src/mame/layout/bzone.lay`:
  memory map, input polarities, DIP defaults, AVG datapath, the discrete
  sound netlist, the overlay split.
- Eric Smith, `mathdis` (microword field decode, GPL, read only).
- 6502disassembly.com "Atari Math Box" (3 MHz clock, busy flag).
- tcottrill, "BattleZone Full Disassembly and C Port", `HARDWARE_NOTES.md`
  and `MATHBOX_NOTES.md`, as a cross-reference index into the Atari files.
- AMD Am2901 data sheet (source/function/destination tables, P/G carries).

## 2. Board

- 6502 at 12.096 MHz / 8. A15 is not decoded, so `$7FFA` vectors serve
  `$FFFA` (MAME `map.global_mask(0x7fff)`).
- NMI = 3 kHz (12.096 MHz / 4096) / 12 = 246.09 Hz, gated off by the
  self-test switch on IN0 D4 (MAME `bzone_interrupt`, `BZSTST.MAC` PWRON).
- IN0 `$0800`: D0 right coin, D1 middle coin, D2 left coin (`COIN65.MAC`
  with `COIN01=1`; MAME labels D2 unused), D3 slam (low-true, `SLAM=0`),
  D4 self-test, D5 diagnostic step, D6 AVG halted, D7 3 kHz square from
  the CPU clock / 512. The page's two-slot door wires the left slot to the
  middle mech, as the option sheet's "Center (or Left)" says.
- DSW0 `$0A00` / DSW1 `$0C00` per MAME `BZONEDSW0/1`. Factory defaults are
  MAME's (manual) values: 3 tanks, missile at 10000, bonus 15k and 100k,
  English; 2 coins per play. `COIN65.MAC` confirms the coin-mode bit
  meanings.
- Treads, fire and start read through POKEY ALLPOT `$1828`, active high
  (MAME `IN3`). Only START `$20` is read by the game (`BZONE.MAC` `START=20`),
  so the panel has one start button.
- `$1840` sound latch (Kossow's bit list in `bzone_a.cpp`): D6 is the start
  lamp; the page lights the START button from it.
- Watchdog `$1400`, written every NMI by the game. The timeout is the
  Asteroids board's 256 × 3 kHz (≈ 87 ms); no Battlezone-specific figure
  was found, see §7.

## 3. AVG

`retroweb/shared/atari/avg.*`.

- State sequencer: `kNext[op][state]` is the 036408-01 table written out
  from `STATE2.MAC` (don't-cares as 0). Checked byte for byte against the
  assembled `STATE2.SAV` in the same release, whose PROM page has MAME's
  036408-01 CRC, 5903AF03. One state
  per 8 master clocks, so one per CPU cycle.
- Datapath (latches, normalizer, binary and linear scale, stack, DAC
  bits 12-3, intensity select) follows MAME `avg_device` handlers, which
  follow Jed Margolin's notes and the schematics.
- Beam time is real: VCTR costs `0x8000 >> shifts` master clocks, SVEC
  `0x100 >> shifts`, CNTR its normalizer settle. The CPU sees the halt on
  IN0 D6 only when the picture is actually done.
- Battlezone clip window: STAT with D10 = 0 loads the high/low clip
  switches from D9, and the next opcode latches the beam as a window corner
  (MAME `avg_bzone_device`). GO clears the window.
- The 580 × 400 picture is MAME's `bzone` visarea; the page stretches it to
  the 4:3 tube.

## 4. Math Box

`retroweb/shared/atari/mathbox.*`.

- Four Am2901 slices as one 16-bit ALU with a rippled carry, separate
  I2 for the high and low byte pairs, D and Y tied to the host bus with
  D7 feeding D15, the N xor V sign mux for ASR and JPL, CADD inverting I1
  after a 0 falls out of Q, LDAB jump latch, STALL (all `MBUDOC.DOC`).
- Microword bit order: M1 holds bits 23-20 and F1 bits 3-0. `MBUCOD.V05`'s
  `$OUT` macro puts word bits 23-16 on page `$6800`, and the PROM CRCs
  (MAME's 036175..036180) match those pages' nibbles in that order.
  `mathdis` reads the files in the opposite order.
- Clock 12.096 MHz / 4 = 3.024 MHz, two microcycles per CPU cycle
  (6502disassembly.com "3 MHz"). `$1800` D7 reads 1 while the sequencer
  runs, as `MBUDOC.DOC` and `BZONE.MAC`'s `WAIT` loop expect; MAME returns
  0 always. The longest documented routine (131 + 5N, N = 16) is ~70 CPU
  cycles, inside `MBDIAG.MAC`'s 99-cycle timeout.
- Check: Atari's real microcode run through this model against MAME's
  hand-written `mathbox_device::go_w` on about 175,000 random operand sets for
  commands `$0B $11 $12 $13 $14 $1D $1E`: zero mismatches. The only
  divergence was `$1E` called directly with negative operands, where
  MAME's C model shifts arithmetically and the microcode shifts logically;
  the game only reaches `$1E` through `$1D`, which makes them positive.
  That check ran locally against a dump and isn't in CI.
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

`battlezone/sound.*` ports MAME's `bzone_a.cpp` netlist node by node:
the 16-bit LS164 noise LFSR at 6 kHz with its /2 and NAND4 taps, the
4066 `RC_CIRCUIT_1` gates for shell (R14+R15, C9) and explosion
(R16+R17, C14), the switched-gain op-amp filters, the engine 555 VCO with
its RCDISC3 control voltage and the F4/F5 counters, and the final
resistor mix with C20 coupling. The R11 engine pot sits at MAME's 40 %
default.

- POKEY's AUDIO pin is open drain into a 10 k pull-up and 0.015 µF
  (MAME `bzone_audio` `set_output_rc`). `shared/atari/pokey.*` models it
  as MAME's `RC_LOWPASS` output: each channel's volume bits switch in
  90 k / 26.5 k / 8.05 k / 3.4 k to ground (MAME `vol_init`), and the cap
  is stepped every chip clock and averaged per output sample. One channel
  at full volume swings the pin about 4 V, so the siren and blips sit at
  their board level against the discrete sounds. A linear 0..5 V scale
  gave one channel 1.25 V. Centipede and Missile Command keep the linear
  output.
- D5 (sound enable) mutes past C20, as MAME's `system_mute` does. Gating
  the mixer input instead let C20 turn the DC step into a pop at every
  game start and game over. How the board itself gates D5 isn't in the
  sources above.

- MAME feeds the shell filter's loud switch from D1 (explosion loud). The
  shell uses D3 here: Kossow's bit list and the game's `LOUDSH=8` both say
  D3 is the shell's loud/soft bit.

## 6. Self-test ROM

`roms/hwtest/gen_hwtest.py` builds an original program, vector ROM and
Math Box microcode under Atari's chip names:

- Signature `BZN1` at `$0000`.
- Its own microcode: byte loads that keep the other half (split I2),
  an unrolled CADD/RAMQD multiply with the N xor V sign mux, and a signed
  max using LDAB + JPL. The 6502 waits on the busy bit, checks the
  product and the max, and shows MATH BOX OK or FAIL.
- AVG diagnostic: border, crosshair, three boxes, an intensity ramp and a
  diagonal cut by the clip window. Then the help screen in this repo's
  stroke font, title and SELF TEST OK under the red overlay.
- Main loop starts the AVG once every six NMIs, after IN0 D6 says the last
  picture is done, as Battlezone does.

## 7. Departures and open items

- High-score RAM `$0300–$033B` persists in IndexedDB for user ROMs (real
  PCBs have no battery). Reset HIGH SCORE clears it.
- The AVG clip switches start "on" (no window pending) at power-on. MAME
  starts them off, which latches a degenerate window on the first opcode
  after reset; the real power-on state isn't documented.
- The 036408 state PROM in a user's set is ignored, since `kNext` is the
  same table.
- Watchdog period is borrowed from the Asteroids board.
- The rev 2 (`bzone`, 036414-02) high-score table slants left and its
  last line can touch BONUS TANK. That's the game: rev 2 steps between
  lines with one relative vector of (-268, -40) after 11 characters of
  24 units (264), so each line starts 4 units further left
  (6502disassembly.com Battlezone rev 2 at `$552F`). Rev 1 (`bzonea`)
  centres the beam before every line, as `BZONE.MAC` `DISTBL` does, and
  draws it straight.
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
