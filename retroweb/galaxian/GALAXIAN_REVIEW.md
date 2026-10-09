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
- Vblank **NMI**, held on the flip-flop until `$7001` D0 is written 0
  (MAME `irq_enable_w`). The game clears and re-arms it in its handler.
- Discrete analog: the MAME `galaxian_a.cpp` netlist, component by
  component. See §9.

## 3. Shared chips

`retroweb/shared/cpu/cpu_z80.{h,cpp}` and `retroweb/shared/galaxian/video`.
`Board::Galaxian`: starfield that drifts one RNG step a frame and no 555
blink, four-pixel white shells and a yellow missile, no Scramble blue
backdrop, no Frogger nibble-swap/river. ISA tests run as
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
| `$6002` | coin lockout (D0 low locks) |
| `$6003` | coin counter |
| `$6004–$6007` | LFO frequency DAC |
| `$6800` | IN1 |
| `$6800–$6807` | 74LS259 sound (FS1/2/3, HIT, FIRE, VOL1/2) |
| `$7000` | IN2 |
| `$7001` | NMI enable |
| `$7004` | stars enable |
| `$7006` / `$7007` | flip X / Y |
| `$7800` | watchdog **read**; fire pitch **write** |

## 5. Video

Galaxian tilemap, 32×32 of 8×8, 2bpp, drawn by the beam (shared
`video.cpp`, see `SCRAMBLE_REVIEW.md` §10 for the family write-up).
Stars (17-bit LFSR, two RNG clocks per pixel) with no Scramble 555 blink;
the field drifts one step a frame. Seven shells and one missile in object
RAM at `$60`: one shell and one missile per line, shells white, missile
yellow, **four** pixels, OR'd into the RGB. Color PROM 6L through MAME's
resnet weights (R/G 1 kΩ/470 Ω/220 Ω, blue 470 Ω/220 Ω, 470 Ω pulldown).

## 6. Inputs / DIPs

Active-high, 2-way. Cocktail P2 unmapped (same labelled skip as the other
arcade machines). The lockout coil turns coins away while `$6002` D0 is
low; the game unlocks it during boot. Start lamps not modeled.

IN0: coin1 `$01`, coin2 `$02`, L/R `$04/$08`, fire `$10`, cabinet `$20`.
IN1: start1/2 `$01/$02`, coinage bits 7:6 (default 00 = 1C/1C).
IN2: bonus bits 1:0 (default 00 = 7000), lives bit 2 (default 1 = 3 lives).

## 7. User ROM

Optional. `roms/user/`, `GALAXIAN_ROM`, `~/images/arcade/galaxian.zip`, or
`~/Downloads/galaxian.zip`. Size-check only, MAME `galaxian` / `galaxiana`
chip names. IndexedDB `retroweb-galaxian`. CI never has a Namco dump, so
playthrough tests skip there.

## 8. Known simplifications

- Cocktail P2 stick unmapped; start lamps not modeled.
- The noise flip-flop clock. MAME names it 2V but generates it at
  60*264/2 Hz, which is 1V's rate. We keep MAME's rate, derived from the
  real line timing (8000 Hz). Which net it really is needs the schematic.
- **HIGH SCORE RAM is volatile on the real PCB.** There is no battery.
  `$40A8` (3 BCD HI-SCORE, `hiscore.dat`) dies on power-off. This page always
  persists those bytes in IndexedDB for a user ROM — a labelled departure.
  Restore once NMI is on, frames ≥ 60, and HI-SCORE is still the factory
  zeros (POST junk must not mark the table "already restored").
  **Reset HIGH SCORE** deletes the save and `machine.reset()`s the board.
  Covered by `web/tests/hiscore.spec.ts`.


## 9. Parity fixes (2026-10-09)

Items G1, V1-V7, B1-B3 and X3 from `shared/cpu/Z80_ARCADE_PARITY.md`.

- **Discrete sound (G1).** *Fact:* the board's sound is analog: a 4-bit
  R1 ladder sets a PNP current source charging C15 under a 555, whose ramp
  becomes the control voltage for three 555 VCOs (FS1-3); two LS164s
  divide 1.536 MHz by `256 - pitch` into a 74393; HIT gates C21 with the
  RNG through an MFB band-pass; FIRE sweeps a 555 by R47/C28 and gates C25.
  *Why:* the old model invented its frequencies and envelopes. *What:*
  `sound.cpp` ports each block at 192 kHz: ladder Millman sum, 555 charge
  curves with exact crossing times, the band-pass as a bilinear MFB
  biquad, the two resistor mixers, and the C26/C46 coupling. The RNG runs
  at its real 12.288 MHz and is latched once per 192 sound clocks; MAME
  runs it at 1/100 speed for performance. R40 uses the schematic's 2.2 kΩ,
  not MAME's 0.6 "volume adjust". Output is volts, MAME's 1 V full scale.
  *Source:* MAME `galaxian_a.cpp` (Couriersud) and the `disc_*` primitives.
  `DiscreteSound.*` (12 tests).
- **Held NMI.** See §2. The self-test ROM's handler now clears and re-arms
  `$7001` as the real game does. `Machine.VblankNmiHoldsUntilTheEnableLatchIsCleared`,
  `Machine.VblankNmiFiresEveryFrameWhenRearmed`.
- **Watchdog keeps RAM (B1)**, `Machine.WatchdogResetKeepsRam`.
- **Coin lockout and counter (B3).** MAME `coin_lock_w`, `coin_count_0_w`.
  `Machine.CoinLockoutTurnsCoinsAway`, `Machine.CoinCounterCountsRisingEdges`.

The real `galaxian` set boots, takes a coin, starts and fires
(`play_test`), and frames dumped from it look right.

