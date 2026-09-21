# Frogger arcade board — design notes

Citation trail for the Konami Frogger (1981) emulator. MAME `galaxian.cpp`
(machine `frogger`) is a cross-check of decoded chip-selects, not a behavior
source — same rule as Pac-Man vs. `pacman.cpp`.

## 0. Status

Dual Z80 + Galaxian video + AY-3-8910 + two i8255s, covered by GoogleTest
(`make check`) and a generated hardware self-test ROM. The Emscripten front
end ships the self-test; a real Konami/Sega `frogger` set is opt-in and
browser-local. Landing-page card (286×128 Courier New starfield tile from
`retroweb/shared/marquee.py`) and CI jobs (`frogger-test` /
`frogger-web-test`) are wired in. The Z80 core lives in
`retroweb/shared/cpu/`. Generic Galaxian/Konami chips (AY, 8255, video
raster, sound-board timer) live in `retroweb/shared/galaxian/` and are
shared with Scramble; Frogger keeps its own map, D0↔D1 swaps, nibble-swap,
river split, and timer bits 3↔5.

## 1. Scope

Konami GX392 / Sega-licensed US upright, 1981 only. Not Frogger II, Amidar,
Super Cobra, or bootlegs.

## 2. Clocks (never sped up on the live page)

- Master 18.432 MHz.
- Main Z80 18.432 / 6 = **3.072 MHz**. 50,688 T-states/frame @ 60.606 Hz.
- Sound Z80 and AY-3-8910 14.31818 / 8 = **1.789772 MHz**.
- Raster 384×264; visible 256×224 native; upright **ROT90** → canvas 224×256.
- Vblank **NMI** on the main CPU, gated by `$B808` D0.
- Sound `/INT` on the falling edge of PPI1 port B bit 3.

## 3. Shared Z80

`retroweb/shared/cpu/cpu_z80.{h,cpp}`. Two `z80::Cpu` instances, each with
its own `Bus`. ISA tests run as `make -C retroweb/shared/cpu check` (named
GoogleTests plus zexdoc). This board smokes that CPU in
`Smoke.HwtestBootsSignatureWatchdogAndPaints`.

## 4. Memory map

Computer Archaeology Frogger hardware notes; Konami/Sega service material.

| Range | Contents |
|---|---|
| `$0000–$3FFF` | main program ROM |
| `$8000–$87FF` | work RAM |
| `$8800` | watchdog **read** (returns `$FF`) |
| `$A800–$ABFF` | tile VRAM |
| `$B000–$B0FF` | object RAM (column scroll/colour, 8 sprites, unused bullets) |
| `$B808 / $B80C / $B810` | NMI enable, flip Y, flip X |
| `$D000 / $D002` | PPI1 sound latch / control |
| `$E000 / $E002 / $E004` | PPI0 IN0 / IN1 / IN2 (active-low) |

Sound CPU: ROM `$0000–$17FF`, RAM `$4000–$43FF` (mirrored through `$5FFF`).
AY I/O is bit-decoded: bit 7 = address, bit 6 = data (so `$80` / `$40`).
AY port A is the command latch; port B is the Konami sound-board timer
(Frogger swaps bits 3 and 5 of that reading). First 2K of sound ROM (608)
and gfx ROM 606 (plane 1 at `$800`) have D0↔D1 swapped on the PCB —
applied at `load_roms`. 607 is the high plane.

## 5. Video

Galaxian tilemap, 32×32 of 8×8, 2bpp (607 at `$000` is the high plane,
606 at `$800` the low plane). Each byte is MSB-left (bit 7 = native left) so that after
ROT90 the Konami glyphs read upright. Per-column scroll and sprite Y
enter the adder with nibbles swapped (PCB wiring). A hardware colour split paints the river: native
x < 128 is blue (`0x47`), the rest black — tile/sprite pen 0 is
transparent onto that. Eight 16×16 sprites; the first three match V−1.
Color PROM is 32 bytes; Frogger's blue gun has PROM bit 0 unconnected.
Attribute bits are remapped `((attr >> 1) & 3) | ((attr << 2) & 4)` —
PROM wiring, cross-checked against MAME `frogger_extend_tile_info` /
`frogger_adjust`.

## 6. Sound

One AY-3-8910. PPI1 port A is the command latch, read back on AY I/O A.
Bit 4 of the control port mutes. Discrete analog filter on the real PCB
is not modeled (dry mix) — documented simplification.

## 7. Inputs / DIPs

IN0: L/R `$10/$20`, coins `$40/$80`, service `$04`.
IN1: lives DIP bits 1:0 (default 00 = 3), starts `$40/$80`.
IN2: U/D `$10/$40`, coinage bits 2:1, cabinet bit 3.
Idle factory: IN0 `$FF`, IN1 `$FC`, IN2 `$F1`.

## 8. Test ROM vs. copyrighted ROMs

Ships `roms/hwtest/gen_hwtest.py` — original font and copy, not Konami's.
A user `frogger` set stays in IndexedDB (`retroweb-frogger`). Size-check
only; CRC labels hwtest vs user. Native `play_test` skips unless
`roms/user/`, `FROGGER_ROM`, or `~/Downloads/frogger.zip` holds a dump.
`UserRomInsertsCoinStartsAndHops` coins in, starts, and hops UP until
the frog at `$8044`/`$8047` moves and P1 score / furthest-row tick.

## 9. Known simplifications

- Cocktail P2 stick unmapped.
- Coin-counter solenoids not modeled.
- Galaxian starfield not populated on this game (Frogger uses the river
  colour split instead).
- AY output is a dry mix (no discrete filter).
