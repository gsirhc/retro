# Frogger arcade board — design notes

Konami Frogger (1981) under `retroweb/frogger/`. Fidelity rules follow
`CLAUDE.md` and `.claude/arcade.md`.

## Sources

- Sega/Gremlin Frogger 834-0068 (video) and 834-0069 (sound).
- GI AY-3-8910/8912 data sheet. Intel 8255A data sheet.
- Computer Archaeology Frogger hardware notes, for the RAM map.
- Shared video and the Konami output stage: `SCRAMBLE_REVIEW.md` §10.
  Frogger's own pieces are the map, the D0↔D1 swaps, the nibble swap,
  the river split and timer bits 3↔5.

## 0. Status

Dual Z80, Galaxian video, one AY-3-8910 and two 8255s. GoogleTest
(`make check`) and a generated hardware self-test ROM. The page ships
the self-test. A real Konami/Sega `frogger` set is opt-in and
browser-local. The Z80 core is `retroweb/shared/cpu/`. Shared chips
live in `retroweb/shared/galaxian/`.

## 1. Scope

Konami GX392 / Sega-licensed US upright, 1981. Not Frogger II, Amidar,
Super Cobra, or bootlegs.

## 2. Clocks

- Master 18.432 MHz. Main Z80 is 18.432 / 6 = 3.072 MHz. 50,688
  T-states a frame, 60.606 Hz.
- Sound Z80 and the AY are 14.31818 / 8 = 1.789772 MHz.
- Raster 384×264. Visible 256×224, upright on a 224×256 canvas.
- Vblank NMI on the main CPU, gated by `$B808` D0.
- Sound /INT is the falling edge of PPI1 port B bit 3. The flip-flop
  clears on M1 + IORQ (834-0069, LS74 at 11).

## 3. Memory map

| Range | Contents |
|---|---|
| `$0000–$3FFF` | main program ROM |
| `$8000–$87FF` | work RAM |
| `$8800` | watchdog read (returns `$FF`) |
| `$A800–$ABFF` | tile VRAM |
| `$B000–$B0FF` | object RAM |
| `$B808 / $B80C / $B810` | NMI enable, flip Y, flip X |
| `$B818 / $B81C` | coin counters 1 / 2 |
| `$D000 / $D002` | PPI1 sound latch / control |
| `$E000 / $E002 / $E004` | PPI0 IN0 / IN1 / IN2 (active low) |

Sound CPU: ROM `$0000–$17FF`, RAM `$4000–$43FF` (mirrored through
`$5FFF`). Filter latch `$6000–$6FFF`, mirrored to `$7FFF`, takes its
data from the address lines. AY I/O is bit-decoded on 834-0069 at 37/39:
bit 7 is address, bit 6 is data. AY port A is the command latch. Port B
is the sound-board timer with bits 3 and 5 swapped.

The first 2K of sound ROM (608) and gfx ROM 606 (plane 1 at `$800`) have
D0↔D1 swapped on the PCB, applied in `load_roms`. 607 is the high plane.

## 4. Video

Galaxian tilemap, 32×32 of 8×8, 2 bpp. 607 at `$000` is the high plane,
606 at `$800` the low plane. Bytes are MSB-left so the glyphs read
upright after the rotation. 834-0068: HPLA0–2 reach the colour inputs
through the LS175 at 43 and the LS377 at 61, which is the attribute
remap `((attr >> 1) & 3) | ((attr << 2) & 4)`. The LS283s at 76/77 add
the scroll nibbles swapped. Native x < 128 is the river, an LS126 and
470 Ω of blue, and tile pen 0 is transparent onto it. Eight 16×16
sprites. The first three match one line early. Shells are four white
pixels. No starfield.

## 5. Sound

One AY-3-8910. Tone period is 16 × TP input clocks. The DAC is 1 V at
level 15 and 3 dB a step (GI data sheet), 0 V at level 0. Each channel
then goes through the 834-0069 network: 1 kΩ, a 4066 (470 Ω on) switching
0.22 µF and 0.047 µF, 5.1 kΩ into an LM741 with 5.6 kΩ feedback, then
0.15 µF into the amp. One full-level channel is scaled to 0.5 peak to
peak, and the page plays that at unity. Bit 4 of the control port mutes.

## 6. Inputs

IN0: L/R `$10/$20`, coins `$40/$80`, service `$04`.
IN1: lives bits 1:0 (default 00 = 3), starts `$40/$80`.
IN2: U/D `$10/$40`, coinage bits 2:1, cabinet bit 3.
Idle factory: IN0 `$FF`, IN1 `$FC`, IN2 `$F1`.

## 7. Unsourced

- Cocktail player 2's stick is unmapped.
- 4066 on-resistance and the LM741 swing are typical data-sheet values.
- High score `$83EF–$83FA` (the display word and five 16-bit ranks) has
  no battery. The page keeps those bytes in IndexedDB for a user ROM.
  Reset HIGH SCORE deletes the save and resets the board.

## ROM policy

Ships a from-scratch hwtest ROM. A user `frogger` set stays in IndexedDB
(`retroweb-frogger`). Size-checked. Native `play_test` skips unless
`roms/user/`, `FROGGER_ROM`, or `~/Downloads/frogger.zip` holds a dump.

## Tests

- `make -C retroweb/frogger check`
- `make -C retroweb/frogger/web test` (Playwright on :8600)
