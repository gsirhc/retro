# Galaxian arcade board — design notes

Namco Galaxian (1979) under `retroweb/galaxian/`. Fidelity rules follow
`CLAUDE.md` and `.claude/arcade.md`.

## Sources

- Midway logic schematic A082-91352-A000. Discrete sound is page 22
  (`galaxian/sound.*`). Video timing and the colour resistors are the
  same sheet family, modelled in `retroweb/shared/galaxian/video.*`
  (`Board::Galaxian`).
- The shared video write-up is `SCRAMBLE_REVIEW.md` §10.

## 0. Status

Single Z80, Galaxian video and discrete analog sound. GoogleTest
(`make check`) and a generated hardware self-test ROM. The page ships
the self-test. A real Namco `galaxian` set is opt-in and browser-local.
The Z80 core is `retroweb/shared/cpu/`.

## 1. Scope

Namco parent set (chips `galmidw.u` / `v` / `w` / `y`, `7l`, `1h.bin`,
`1k.bin`, `6l.bpr`) and the 4K+4K+2K split (`7f.bin` / `7j.bin` /
`7l.bin`). Not Super Galaxian, clones with extra RAM, or the later
dual-Z80 boards (`/frogger/` and `/scramble/`).

## 2. Clocks

- Master 18.432 MHz. Z80 is 18.432 / 6 = 3.072 MHz. 50,688 T-states a
  frame, 60.606 Hz.
- Raster 384×264. Visible 256×224, upright on a 224×256 canvas.
- Vblank NMI holds until `$7001` D0 is written 0 (9N Q1 presets 6F; low
  holds NMI off). The game clears and re-arms it in the handler.
- Sound clock is 2H, 18.432 MHz / 12 = 1.536 MHz.

## 3. Memory map

| Range | Contents |
|---|---|
| `$0000–$3FFF` | program ROM (10K populated on the parent) |
| `$4000–$43FF` | work RAM, mirrored `$0400` |
| `$5000–$53FF` | tile VRAM, mirrored `$0400` |
| `$5800–$58FF` | object RAM, mirrored `$0700` |
| `$6000` | IN0 (active high) |
| `$6002` | coin lockout (9M Q2, low locks) |
| `$6003` | coin counter |
| `$6004–$6007` | background ramp DAC (9M Q4–Q7) |
| `$6800` | IN1 |
| `$6800–$6807` | 74LS259 sound (FS1/2/3, HIT, FIRE, VOL1/2) |
| `$7000` | IN2 |
| `$7001` | NMI enable |
| `$7004` | stars enable |
| `$7006` / `$7007` | flip X / Y |
| `$7800` | watchdog read; fire pitch write (9J) |

## 4. Video

Tilemap 32×32 of 8×8, 2 bpp, drawn as the beam scans. Stars are a 17-bit
LFSR (tap on 3B, A1 QE), two clocks a pixel, no 555 blink. The field
drifts one step a frame. Seven shells and one missile in object RAM at
`$60`: one of each per line, shells white, missile yellow, four pixels,
OR'd into the RGB.

Colour PROM 6L drives 1 kΩ / 470 Ω / 220 Ω on red and green and 470 Ω /
220 Ω on blue, into 470 Ω. A star bit drives 150 Ω / 100 Ω through the
74366. Levels are the TTL high (3.4 V) into that network. See §10 of
the Scramble notes for sprites, shells and the beam.

## 5. Sound

`sound.*` is page 22 of A082-91352, stepped at 192 kHz.

- Background: 9M Q4–Q7 through R18/R17/R16/R15 (1 MΩ, 470 kΩ, 220 kΩ,
  100 kΩ) into Q1, then the 555 ramp on C15. 7T sets the control voltage.
- FS1–3: 555s, Ra 100 kΩ, Rb 470 kΩ / 330 kΩ / 220 kΩ, C 0.01 µF.
- Hit: noise through the MFB band-pass.
- Fire: a 555 swept by R47/C28.
- Pitch: 9J latches `256 - pitch` into the divider off 1.536 MHz.
- Mix is the LM377 input across R92, in volts.
- Noise is 7474 2D. 2V is wired to its clock pin, so the star shift
  register is sampled on that rising edge, once per four lines.

## 6. Inputs

Active high, 2-way. Cocktail player 2 is unmapped. `$6002` D0 low locks
the coin chute. Start lamps are not modelled.

IN0: coin1 `$01`, coin2 `$02`, L/R `$04/$08`, fire `$10`, cabinet `$20`.
IN1: start1/2 `$01/$02`, coinage bits 7:6 (default 00 = 1 coin 1 play).
IN2: bonus bits 1:0 (default 00 = 7000), lives bit 2 (default 1 = 3).

## 7. Unsourced

- TTL, 555 and LM324 levels are typical data-sheet values.
- High score at `$40A8` (3 BCD bytes) has no battery. The page keeps
  those bytes in IndexedDB for a user ROM. Restore waits until NMI is
  on and the bytes are still the factory zeros. Reset HIGH SCORE deletes
  the save and resets the board. Source: Tunstall's Galaxian ROM
  disassembly (github.com/ScottTunstall/Galaxian). The self-test path
  clears work RAM at `$1B8A`, turns NMI on at `$1BC7`, and only
  `UPDATE_HIGH_SCORE` (`$21E9`) writes `$40A8` after that.

## ROM policy

Ships a from-scratch hwtest ROM. User sets (a `galaxian` zip, or the
loose chips named above) load into IndexedDB `retroweb-galaxian`.
Size-checked. CI has no Namco dump, so the playthrough test skips.

## Tests

- `make -C retroweb/galaxian check`
- `make -C retroweb/galaxian/web test` (Playwright on :8800)
