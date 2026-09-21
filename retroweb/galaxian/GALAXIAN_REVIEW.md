# Galaxian arcade board — design notes

Citation trail for the Namco Galaxian (1979) emulator. MAME `galaxian.cpp`
(`galaxian_map_base` + `galaxian_map_discrete`) is a cross-check of decoded
chip-selects, not a behavior source — same rule as Frogger/Scramble.

## 0. Status

Single Z80 + Galaxian video + discrete analog sound, covered by GoogleTest
(`make check`) and a generated hardware self-test ROM. The Emscripten front
end ships the self-test; a real Namco `galaxian` set is opt-in and
browser-local. Landing-page card (286×128 Courier New starfield tile from
`retroweb/shared/marquee.py`) and CI jobs (`galaxian-test` /
`galaxian-web-test`) are wired in. Shared video lives in
`retroweb/shared/galaxian/` (`Board::Galaxian`); the Z80 core is
`retroweb/shared/cpu/`.

## 1. Scope

Namco parent set `galaxian` (chips `galmidw.u`/`v`/`w`/`y`, `7l`, `1h.bin`,
`1k.bin`, `6l.bpr`) and the 4K+4K+2K `galaxiana` split (`7f.bin`/`7j.bin`/
`7l.bin`). Not Super Galaxian, clones with extra RAM, or Konami's later
dual-Z80 children (those are `/frogger/` and `/scramble/`).

## 2. Clocks (never sped up on the live page)

- Master 18.432 MHz.
- Z80 18.432 / 6 = **3.072 MHz**. 50,688 T-states/frame @ 60.606 Hz.
- Raster 384×264; visible 256×224 native; upright **ROT90** → canvas 224×256.
- Vblank **NMI**, gated by `$7001` D0.
- Discrete analog: pitch is LS164s at `SOUND_CLOCK/(256−pitch)` mixed
  always (the dive/bomb whistle); FIRE is a decaying 555 shot; HIT is
  LFSR noise through `(R35+R36)·C21`. Labelled RC-time-constant model,
  not a SPICE netlist. MAME `galaxian_a.cpp` is a latch/RC cross-check.

## 3. Shared chips

`retroweb/shared/cpu/cpu_z80.{h,cpp}` and `retroweb/shared/galaxian/video`.
`Board::Galaxian`: starfield with no 555 blink, four-pixel yellow bullets,
no scramble blue backdrop, no Frogger nibble-swap/river. ISA tests run as
`make -C retroweb/shared/cpu check`; this board smokes that CPU in
`Smoke.HwtestBootsSignatureWatchdogAndPaints`.

## 4. Memory map

Schematic-derived Namco parent map. MAME `galaxian_map_*` is a cross-check.

| Range | Contents |
|---|---|
| `$0000–$3FFF` | program ROM (10K populated on the parent) |
| `$4000–$43FF` | work RAM, mirrored `$0400` |
| `$5000–$53FF` | tile VRAM, mirrored `$0400` |
| `$5800–$58FF` | object RAM, mirrored `$0700` |
| `$6000` | IN0 (active high) |
| `$6004–$6007` | LFO frequency DAC |
| `$6800` | IN1 |
| `$6800–$6807` | 74LS259 sound (FS1/2/3, HIT, FIRE, VOL1/2) |
| `$7000` | IN2 |
| `$7001` | NMI enable |
| `$7004` | stars enable |
| `$7006` / `$7007` | flip X / Y |
| `$7800` | watchdog **read**; fire pitch **write** |

## 5. Video

Galaxian tilemap, 32×32 of 8×8, 2bpp. Stars (17-bit LFSR) with no Scramble
555 blink. Eight shells in object RAM at `$60` (yellow, **four** pixels).
Color PROM 6L: R/G 1 kΩ + 470 Ω + 220 Ω, blue **470 Ω + 220 Ω** (no 1 kΩ).
Cross-checked against MAME `galaxian_v.cpp`, not a source.

## 6. Inputs / DIPs

Active-high, 2-way. Cocktail P2 unmapped (same labelled skip as the other
arcade machines). Coin counters and lamps not modeled.

IN0: coin1 `$01`, coin2 `$02`, L/R `$04/$08`, fire `$10`, cabinet `$20`.
IN1: start1/2 `$01/$02`, coinage bits 7:6 (default 00 = 1C/1C).
IN2: bonus bits 1:0 (default 00 = 7000), lives bit 2 (default 1 = 3 lives).

## 7. User ROM

Optional. `roms/user/`, `GALAXIAN_ROM`, `~/images/arcade/galaxian.zip`, or
`~/Downloads/galaxian.zip`. Size-check only, MAME `galaxian` / `galaxiana`
chip names. IndexedDB `retroweb-galaxian`. CI never has a Namco dump, so
playthrough tests skip there.
