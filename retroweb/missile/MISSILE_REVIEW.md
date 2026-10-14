# Missile Command Arcade — review notes

Atari Missile Command (1980) board emulator under `retroweb/missile/`.
Fidelity rules follow `CLAUDE.md` and `.claude/arcade.md`.

## Hardware citations

- Board A035467, clocks, sync chains, IRQ flip-flops F7/E7, MADSEL, 3rd-bit
  scatter, I/O bits, DIP tables, watchdog (8 vblanks), ROM/PROM set:
  MAME `src/mame/atari/missile.cpp` (Aaron Giles, from the schematics).
- RAM usage, high-score table, reset/IRQ flow, R10 coin decode, palette
  polarity: Andy McFadden's rev 3 disassembly,
  https://6502disassembly.com/va-missile-command/MissileCommand.html
  (cross-checks https://github.com/historicalsource/missile-command).

## Clock

- 10 MHz master. Pixel clock 5 MHz, HTOTAL 320, VTOTAL 256: 15.625 kHz
  lines, 61.035 Hz frames.
- CPU is 10 MHz / 8 = 1.25 MHz, but Sigma-X switches to half rate while
  V ≥ 224 (MAME `adjust_cpu_speed`, schematic timing diagram). One frame
  is 224 × 80 + 32 × 40 = 19200 CPU cycles, so the average is
  1,171,875 Hz. MAME's PCB notes measured 1.1724 MHz at the 6502's clock
  pin and called it a mystery. It is the Sigma-X average.
- POKEY runs at a steady 1.25 MHz through the slow lines; audio is mixed
  per scanline, not per CPU cycle.
- The web page paces the board at `kAvgCpuHz` wall-clock. No turbo.

## Interrupts

- F7 latches 32V on each 16V edge (/16V when flipped): asserts at V = 0,
  64, 128, 192. It is level-held until `$4D00` acks it.
- The /IRQ pin only changes at SYNC (opcode fetch). E7 keeps the previous
  pin as /IRQH. The board models both at instruction boundaries; the 6502
  sees the pin latched at the previous SYNC, as it samples IRQ at the end
  of the current instruction.
- The power-on V = 0 edge is clocked at reset, matching MAME's first IRQ
  timer.

## MADSEL

- Loaded at SYNC when the opcode's low five bits are `00001` (every
  `($zz,X)` instruction) and /IRQH is inactive. It goes high five cycles
  later, which is the instruction's data access. The shared core skips
  the zero-page dummy read, so that is its fifth bus access; the board
  counts accesses per instruction.
- Pixel address = the 16-bit pointer, data in bits 7–6 (and bit 5 for the
  3rd bit at `$E000+`). Writes go through the 035826-01 L6 PROM masks.
  A user set's PROM is used when present; otherwise the board uses the
  masks the read logic implies (keep all pixels but the addressed one).
- The 3rd-bit access adds one CPU cycle (MAME `adjust_icount(-1)`).

## Video

- 256 × 231 visible (MAME VBEND 25; line 24 shows garbage). 2bpp plane at
  `y × 64`, nibble-split pixels; lines 224–255 add a 3rd colour bit from
  the scattered plane at `$0200–$05FF`.
- Palette `$4B00–$4B07`: bits 3/2/1 = R/G/B, active low, 1 bit each.
- Flip (OUT0 bit 6 low) reverses V; IRQ edges, Sigma-X and VBLANK all
  follow the flipped V.

## I/O

- `$4800` read: IN0 or, with CTRLD (OUT0 bit 0) set, the 4-bit trackball
  counters as `VVVVHHHH`. Cocktail P2 trackball is unmapped and reads 0.
- `$4900` IN1: VBLANK (V < 24), self-test, slam, fire L/C/R active low.
- R8 options are read through POKEY ALLPOT (`$4008`), R10 at `$4A00`.
- DIP defaults are MAME's (R8 `$73`, R10 `$81`, which is 2 coins 1 play).
  We couldn't find Atari's factory chart to confirm them.

## Departures (labelled)

- POKEY is the shared compact channel model (see `CENTIPEDE_REVIEW.md`).
- High scores live in work RAM `$002C–$005B`. A real board loses them at
  power-off and `HandleReset` re-seeds the defaults. IndexedDB keeps them
  across reloads for user ROMs. Reset HIGH SCORE deletes the save and power
  cycles the board, which brings back the ROM's own default table.
- A watchdog bite resets the CPU, POKEY and IRQ latches but keeps DRAM, as
  /RESET does. The page's power cycle clears DRAM.

## ROM policy

Ships a from-scratch hwtest ROM only. User sets (MAME `missile` zip or the
six loose `035820-02`…`035825-02` chips, optional `035826-01` PROM) load
into IndexedDB. Size-checked; CRC labels hwtest vs user only.

## Tests

- `make -C retroweb/shared/cpu test-smoke-mos6502` + `dormann`
- `make -C retroweb/missile test` (timing, IRQ, MADSEL, I/O, video, audio,
  board smoke + help screen)
- `make -C retroweb/missile/web test-smoke` (Playwright smoke on :9200)
