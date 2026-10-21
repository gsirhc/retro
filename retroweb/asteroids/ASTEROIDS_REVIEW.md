# Asteroids Arcade — review notes

Atari Asteroids (1979) board emulator under `retroweb/asteroids/`.
Fidelity rules follow `CLAUDE.md` and `.claude/arcade.md`.

## Sources

- Atari DP-143 schematic package (sheet 1A/1B MPU, memory map table,
  clocks, watchdog, option switches; sheet 2A vector generator and its
  circuit description; sheet 2B audio). The scan used is the
  `asteroids_schematics` set in the deramp.com MITS/Atari archive.
- Atari TM-143 operator's manual (option switch table, self test).
- Atari's Asteroids source as published in the `historicalsource/asteroids`
  repository: `ASTDEC.XX` (I/O equates, option switch bits, HALT and
  saucer-select polarity), `A35131.1A`-`.1E` (game, page 0 layout, INIT
  option reads, sound routines), `VECMAC.XX` (vector instruction
  encodings), `ASTROD.DOC` (part numbers and PCB locations). Read for
  behaviour only; nothing from those files is in this repo.
- Jed Margolin, "The Secret Life of Vector Generators" (jmargolin.com),
  for the DVG normalization and timer description.
- Philips NE566 data sheet (triangle output swing).
- Rev 4 program ROM (`035145-04e`, `035144-04e`, `035143-02`),
  disassembled for the high-score table (High score below).

## Board

- 6502 at 12.096 MHz / 8. A15 is not decoded.
- Reads: `$2001`-`$2007` and `$2400`-`$2407` drive D7 only through LS251s,
  `$2800`-`$2803` drive D1/D0 from the option switches. The data bus has
  10K pull-ups, so undriven bits read 1.
- Writes: L6 LS42 decodes A11-A9 into `$3000` DMAGO, `$3200` OUT (lamps,
  RAMSEL, coin counters), `$3400` WDCLR, `$3600` EXPLODE, `$3800` DMACNT,
  `$3A00` THUMP, `$3C00` AUDIO (M10 LS259), `$3E00` NOISERESET.
- NMI: C5 LS161 divides 3 kHz by 12; TEST holds its enable low.
- Watchdog: D5 LS393 counts 128 periods of 3 kHz (about 43 ms) and resets.
- `$2002` D7 is the DVG HALT line: 1 while drawing, 0 once halted
  (`ASTDEC.XX`).
- Option switches: `$2800` coinage (SW8/SW7), `$2801` right mech (SW6/SW5),
  `$2802` SW4 coin mode and SW3 lives (3 when set, 4 when clear, `INIT`),
  `$2803` language. Factory `0x12` is the TM-143 suggested setting.
- High-score RAM `$1D`-`$51`: `HSCORE`, `UPDINT`, `UPDFLG`, `INITL` in the
  `A35131.1A` page 0 order, based on the rev 4 ROM.

## Vector generator

`retroweb/shared/atari/dvg.*` runs the DVG at the instruction level from
`VECMAC.XX` encodings and the DP-143 sheet 2A description:

- 12-bit position counters; XVLD/YVLD blank the beam outside 0-1023.
- M6 adds the global scale to the opcode; E7 decodes the sum, and sums of
  10-15 leave the timer at its maximum count, so the beam does not move.
- SVEC (ALPHANUM) decodes bits 11 and 3 into 2, 4, 8 or 16 units a step.
- The stack is F4/H4/J4, four 12-bit words.
- The C8 state PROM (`034602-01`) is not needed and is ignored if a dump
  includes it.

## Sound

`asteroids/sound.*` is a component-level model of DP-143 sheet 2B:

- Thump: N7 555 rate set by the THUMP nibble through R45-R47/R49, gated by
  D4, into R83.
- Saucer: 555 warble into the P11 566, R69 vs R69 || R68 for the large and
  small saucer, enabled by SAUCRSNDEN.
- Ship and saucer fire: Q3/Q1 ramps on C38/C47 pull the 555 rate down as
  the level decays; each 555's reset is its enable.
- Explosion: P7 LS174 latches level bits into R40/R43/R41/R42 and the P8
  LS161 picks the noise sample rate (/12, /6, /3, /5).
- Noise: P10/R10 LS164 at 12 kHz, XNOR of stages 7 and 15, cleared by
  NOISERESET.
- Thrust: noise through the R11 4016 into the R76/R100/C67/C68 band-pass.
- Life: the 3 kHz clock gated by LIFEEN into R78.
- Mixer: P11 summing node around +5V with R83, R77, R78, R81, R84, R82,
  R102, then C113/R111.

## High score

Derived by disassembling the rev 4 program ROM (`$6800-$7FFF`; a local
dump, not the repo).

- Init: RESET (`$7CF3`) zeroes page 0 and `$0100-$03FF` and never copies a
  default table, so the factory table is all zero. It then sets `$32` and
  `$33` to `$B0`.
- Layout: ten 2-byte scores, low then high, at `$1D-$30`, best first. Ten
  3-byte initials at `$34-$51`. The player's score is `$52/$53` (P1) and
  `$54/$55` (P2). `$31` is the shift index, `$32/$33` the per-player
  initials-entry position, `$1C` the check flag. None of those are saved.
- Insert: `$765C` (CheckHighScore), for X = 2 then 0. `$766A` compares the
  score against each table entry (`CPY #$14`, so 10 entries), `$7699`
  shifts the lower scores down and the initials with it (`$76AC`), stores
  the score, and puts `$0B,0,0` in the new initials slot.
- On-screen: `$73C4` draws the table, skipping entries whose two score bytes
  are 0. There is no separate top-score variable.
- Saved: `$1D-$30` and `$34-$51`, 50 bytes.
- Ready and factory: all 50 bytes zero and at least 4 NMIs run. The clear is
  256 passes of 24 cycles at 1.512 MHz, about one NMI at 246 Hz.

## Departures (labelled)

- High-score work RAM is persisted in IndexedDB (real PCBs have no
  battery). Reset HIGH SCORE clears it.
- Vector monitor is the 4:3 tube. Beam X is the full 0..1023; beam Y
  64..960 fills the height so the scores and copyright stay on the glass.

## Unsourced

- The LIFEEN gate between the 3 kHz clock and R78 is not drawn on the
  scan; it is modelled as a TTL level into R78.
- The saucer-fire analog switch and R60 supply lines are unlabeled; they
  are drawn like the ship-fire copy (+5 V and +12 V).
- NE555 output levels 3.3 V / 0.1 V, 4016 on-resistance 470 Ω, LS output
  levels 3.4 V / 0.2 V and the 566 triangle centre are typical data-sheet
  values, not board measurements.

## ROM policy

Ships a from-scratch hwtest ROM only. User sets (the standard `asteroid`
dump zip, or loose `035145` / `035144` / `035143` / `035127` chips) load
into IndexedDB.

## Tests

- `make -C retroweb/shared/cpu test-smoke-mos6502` + `dormann`
- `make -C retroweb/asteroids check` (machine, DVG, sound, board smoke)
- `make -C retroweb/asteroids/web test` (Playwright on :9000)
