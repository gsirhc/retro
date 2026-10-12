# Centipede Arcade — review notes

Atari Centipede (1981) board emulator under `retroweb/centipede/`.
Fidelity rules follow `CLAUDE.md` and `.claude/arcade.md`.

## Hardware citations

- Memory map, IN0–IN3, LS259, EAROM, POKEY, playfield bus steal:
  http://computerarcheology.com/Arcade/Centipede/Hardware.html
- Sprite / playfield RAM field layout, EAROM usage:
  https://6502disassembly.com/va-centipede/Centipede_rev4.html
- Clocks, 32V IRQ, watchdog (8 vblanks), palette RAM, gfx layouts,
  trackball counter:
  MAME `centiped.cpp` / `centiped_v.cpp` (chip-select and video cross-check)
- Atari DP-182 schematics (Centipede PCB)
- CPU: MOS NMOS 6502 @ 1.512 MHz (12.096 MHz / 8). Playfield RAM
  (`$0400`–`$07BF`) halves the CPU clock while accessed. Frame 60 Hz,
  raster 256×256 with 256×240 visible, monitor ROT270 (portrait).

## Shared silicon

- `retroweb/shared/cpu/cpu_mos6502.*` — period NMOS 6502 (same core as
  Asteroids).
- `retroweb/shared/atari/pokey.*` — POKEY at `$1000`–`$100F` (audio +
  RANDOM). Four-channel AUDF/AUDC mix; poly RNG for reads.

## Departures (labelled)

- POKEY mix is a compact channel model, not a full Altirra-grade cycle
  steal / two-tone SIO path. AUDC bits follow MAME (`NOTPOLY5`/`POLY4`/
  `PURE`); pure tone toggles the channel latch. RANDOM must leave SKCTL
  init with a non-zero poly seed (an all-zero 17-bit LFSR sticks); the
  mushroom field is planted from that RNG.
- High-score EAROM (`$1600`–`$163F` data / `$1680` control / `$1700` read)
  is a functional ER2055 (64×8). IndexedDB mirrors the EAROM image so a
  browser refresh keeps scores the way the real chip does across power.
  Reset HIGH SCORE clears the saved image and resets the board.

## ROM policy

Ships a from-scratch hwtest ROM only. User sets (MAME `centiped` zip or
loose `136001-307`…`310` program chips plus `136001-211` / `212` gfx)
load into IndexedDB. Size-checked; CRC labels hwtest vs user only.

## Tests

- `make -C retroweb/shared/cpu test-smoke-mos6502` + `dormann`
- `make -C retroweb/centipede test-smoke` (board smoke)
- `make -C retroweb/centipede/web test-smoke` (Playwright smoke on :9100)
