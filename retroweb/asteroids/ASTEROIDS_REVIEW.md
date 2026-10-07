# Asteroids Arcade — review notes

Atari Asteroids (1979) board emulator under `retroweb/asteroids/`.
Fidelity rules follow `CLAUDE.md` and `.claude/arcade.md`.

## Hardware citations

- Memory map, IN0/IN1/DSW, sound strobes, A15 ignore:
  http://computerarcheology.com/Arcade/Asteroids/Hardware.html
- DVG opcodes (VEC / LABS / HALT / JSR / RTS / JMP / SVEC), scale,
  1024×1024 geometry:
  http://computerarcheology.com/Arcade/Asteroids/DVG.html
- Halt polarity (`$2002` D7 = 1 busy / 0 idle), DIP pair packing at
  `$2800–$2803`, high-score RAM `$1D–$51`, RAMSEL bank swap:
  https://6502disassembly.com/va-asteroids/Asteroids.html
- Clocks / NMI gate / LS259 sound D7 / watchdog ≈87 ms:
  MAME `asteroid.cpp` / legacy `machine/asteroid.c` (cross-check of
  chip-selects, not a behavior source of first resort)
- Atari TM-143 / DP-143 (cabinet, discrete audio)
- CPU: MOS NMOS 6502 @ 1.512 MHz (12.096 MHz / 8), MMI/NMI ≈ 246 Hz
  (12× 3 kHz). Self-test (IN0 bit 7) blocks NMI.

## Shared silicon

- `retroweb/shared/cpu/cpu_mos6502.*` — period NMOS 6502 (not the
  assembler's W65C02S). Undocumented opcodes from Nesdev / Visual6502
  tables; unstable bus-fight ops use documented MAGIC approximations.
  Stock rev-4 game ROM does not require illegals (full set is still
  implemented for the shared core).
- `retroweb/shared/atari/dvg.*` — Digital Vector Generator (shared for
  future DVG boards such as Lunar Lander).

## Departures (labelled)

- Discrete sound uses the DP-143 oscillator rates from MAME's
  asteroid_a.cpp netlist (thump 555 table, ship fire 820→110 Hz,
  saucer warble). The filters are a single-pole mix, not a SPICE model.
- High-score work RAM is persisted in IndexedDB (real PCBs have no
  battery). Reset HIGH SCORE clears it.
- Vector monitor is the 4:3 tube. Beam X is the full 0..1023; beam Y
  64..960 fills the height so the scores and copyright stay on the glass.

## ROM policy

Ships a from-scratch hwtest ROM only. User sets (MAME `asteroid` zip or
loose `035145` / `035144` / `035143` / `035127` chips) load into
IndexedDB. The 256-byte DVG PROM `034602-01.c8` is optional and ignored
(our DVG is ISA-level, not the TTL microcode PROM).

## Tests

- `make -C retroweb/shared/cpu test-smoke-mos6502` + `dormann`
- `make -C retroweb/asteroids test-smoke` (board smoke)
- `make -C retroweb/asteroids/web test-smoke` (Playwright smoke on :9000)
