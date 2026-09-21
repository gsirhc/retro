# Scramble arcade board — design notes

Citation trail for the Konami Scramble (1981) emulator. MAME `galaxian.cpp`
(machine `scramble` / `theend_map`) is a cross-check of decoded
chip-selects, not a behavior source — same rule as Frogger vs. `frogger_*`.

## 0. Status

Dual Z80 + Galaxian video + two AY-3-8910s + two i8255s + PAL 6J, covered
by GoogleTest (`make check`) and a generated hardware self-test ROM. The
Emscripten front end ships the self-test; a real Konami `scramble` set is
opt-in and browser-local. Landing-page card (286×128 Courier New starfield
tile from `retroweb/shared/marquee.py`) and CI jobs (`scramble-test` /
`scramble-web-test`) are wired in. Shared chips (AY, 8255, video raster,
Konami sound-board timer) live in `retroweb/shared/galaxian/`; the Z80
core is `retroweb/shared/cpu/`.

## 1. Scope

Konami parent set `scramble` only (chips `s1.2d`…`s8.2p`, `ot1.5c`…
`ot3.5e`, `c2.5f`/`c1.5h`, `c01s.6e`). Not Super Cobra, The End as a
game, Amidar, Stern `scrambles`, or bootlegs.

## 2. Clocks (never sped up on the live page)

- Master 18.432 MHz.
- Main Z80 18.432 / 6 = **3.072 MHz**. 50,688 T-states/frame @ 60.606 Hz.
- Sound Z80 and both AY-3-8910s 14.31818 / 8 = **1.789772 MHz**.
- Raster 384×264; visible 256×224 native; upright **ROT90** → canvas 224×256.
- Vblank **NMI** on the main CPU, gated by `$6801` D0.
- Sound `/INT` on the falling edge of PPI1 port B bit 3.

## 3. Shared chips

`retroweb/shared/cpu/cpu_z80.{h,cpp}` and `retroweb/shared/galaxian/`
(AY, 8255, tile/sprite/PROM drawing, generic Konami timer). Frogger is a
later PCB that keeps these chips and **moves the map**. ISA tests run as
`make -C retroweb/shared/cpu check` (named GoogleTests plus zexdoc); this
board smokes that CPU in `Smoke.HwtestBootsSignatureWatchdogAndPaints`.

## 4. Memory map

Schematic-derived The End map (Computer Archaeology / Konami service
material). MAME `theend_map` is a cross-check of the decode.

| Range | Contents |
|---|---|
| `$0000–$3FFF` | main program ROM |
| `$4000–$47FF` | work RAM |
| `$4800–$4BFF` | tile VRAM (mirrored through `$4FFF`) |
| `$5000–$50FF` | object RAM (mirrored through `$57FF`) |
| `$6801` | NMI enable |
| `$6803` | scramble background enable |
| `$6804` | Galaxian stars enable |
| `$6806` / `$6807` | flip X / Y |
| `$7000` | watchdog **read** (returns `$FF`) |
| `$8000–$FFFF` | PPI0 if A8, PPI1 if A9 (`theend_ppi8255_*`) |

Sound CPU: ROM `$0000–$1FFF` (6K populated), RAM `$8000–$83FF` (mirrored).
AY I/O is bit-decoded: bits 4–5 = AY2 (addr/data), bits 6–7 = AY1
(addr/data). AY1 port A is the command latch; port B is the **generic**
Konami sound-board timer (no Frogger 3↔5 swap). Tone f = fclock/(16·TP);
envelope step is 16·EP clocks. Filter netlist writes at `$9000` are
ignored — labelled dry-mix simplification, same policy as Frogger §6.

## 5. Video

Galaxian tilemap, 32×32 of 8×8, 2bpp. No nibble-swap, no river split, no
D0↔D1, full PROM blue bits. Galaxian **starfield** (17-bit LFSR) plus a
555 blink (`Ra=100k, Rb=10k, C=10µF`). Scramble background latch at
`$6803` paints a 390 Ω blue. Eight shells in object RAM at `$60` (yellow,
two pixels). Cross-checked against MAME `galaxian_v.cpp` stars/bullets,
not a source.

## 6. Protection PAL at 6J

Nibble in / nibble out on PPI1 port C. Exact PAL equations are not
published. Observed scramble sequences use op `$9` (increment). IN2 bits
5 and 7 feed the “alt” bits derived from the high bit of that nibble.
MAME `theend_protection_*` is a cross-check of those observed ops.

## 7. Inputs / DIPs

Active-low, 8-way. Cocktail P2 unmapped (same labelled skip as Frogger).
Coin counters not modeled.

IN0: bomb `$02`, fire `$08`, R/L `$10/$20`, coins `$40/$80`.
IN1: lives DIP bits 1:0 (default 00 = 3), starts `$40/$80`.
IN2: U/D `$10/$40`, coinage bits 2:1, cabinet bit 3, protection bits 5/7.

## 8. User ROM

Optional. `roms/user/`, `SCRAMBLE_ROM`, or `~/Downloads/scramble.zip`.
Size-check only, MAME `scramble` chip names. IndexedDB `retroweb-scramble`.
CI never has a Konami dump, so playthrough tests skip there.
