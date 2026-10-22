# Scramble arcade board — design notes

Konami Scramble (1981) under `retroweb/scramble/`. Fidelity rules follow
`CLAUDE.md` and `.claude/arcade.md`. Section 10 is the shared
Galaxian-family video and Konami sound, also used by Frogger and Galaxian.

## Sources

- Stern Scramble 33A-105 (sound) and 33A-106 (video).
- GI AY-3-8910/8912 data sheet. Intel 8255A data sheet.
- Computer Archaeology / Konami service notes for the memory map.
- Sound-board divider chain on AY port B (7C/6B/5B LS93, 4B LS90), AY
  chip-select gates (3D: 4A, 6A; 3C: 5A) and the 4066 latch: Scramble
  33A-105 sheets. 40960 crystal clocks per cycle (`shared/galaxian/timer.h`).
- GI AY-3-8910 data manual: BDIR/BC2/BC1 table, I/O pins pulled up.

## 0. Status

Dual Z80, Galaxian video, two AY-3-8910s, two 8255s and PAL 6J.
GoogleTest (`make check`) and a generated hardware self-test ROM. The
page ships the self-test. A real Konami `scramble` set is opt-in and
browser-local. Shared chips live in `retroweb/shared/galaxian/`. The
Z80 core is `retroweb/shared/cpu/`.

## 1. Scope

Konami parent set only (`s1.2d`…`s8.2p`, `ot1.5c`…`ot3.5e`,
`c2.5f` / `c1.5h`, `c01s.6e`). Not Super Cobra, The End as its own
game, Amidar, or bootlegs.

## 2. Clocks

- Master 18.432 MHz. Main Z80 is 18.432 / 6 = 3.072 MHz. 50,688
  T-states a frame, 60.606 Hz.
- Sound Z80 and both AYs are 14.31818 / 8 = 1.789772 MHz.
- Raster 384×264. Visible 256×224, upright on a 224×256 canvas.
- Vblank NMI on the main CPU, gated by `$6801` D0.
- Sound /INT is the falling edge of PPI1 port B bit 3. The flip-flop
  clears on M1 + IORQ (33A-105).

## 3. Memory map

| Range | Contents |
|---|---|
| `$0000–$3FFF` | main program ROM |
| `$4000–$47FF` | work RAM |
| `$4800–$4BFF` | tile VRAM (mirrored through `$4FFF`) |
| `$5000–$50FF` | object RAM (mirrored through `$57FF`) |
| `$6801` | NMI enable (D0 low also clears the held NMI) |
| `$6802` | coin counter |
| `$6803` | background enable |
| `$6804` | stars enable |
| `$6806` / `$6807` | flip X / Y |
| `$7000` | watchdog read (returns `$FF`) |
| `$8000–$FFFF` | PPI0 if A8, PPI1 if A9 |

Sound CPU: ROM `$0000–$1FFF` (6K populated), RAM `$8000–$83FF`. AY I/O
is gate-decoded (33A-105 3D and 3C): a read with A7 (A5 for AY2) set
reads data. A write with A6 (A4) latches the address, otherwise A7 (A5)
writes data. AY1 port A is the command latch. Port B is the divider
chain: B7 LS90 QA, B6 QD, B5 QC, B4 the last LS93 QD; B3–B0 float high.
Frogger moves QC to B3. Filter latch writes at `$9000–$9FFF` (mirror `$6000`) set the
4066 switches from the address lines.

## 4. Video

Galaxian tilemap, 32×32 of 8×8, 2 bpp. No nibble swap, no river, no
D0↔D1 swap. Starfield is the 17-bit LFSR with the tap on 5M (1M QF),
plus a 555 blink (Ra 100 kΩ, Rb 10 kΩ, C 10 µF). `$6803` turns on the
background, an LS126 and 390 Ω of blue. Object RAM at `$60` holds
seven shells on one counter, yellow, two pixels. Slot 7's decode (10M)
is unconnected. Stars do
not drift. See §10.

## 5. Protection PAL at 6J

Nibble in, nibble out, on PPI1 port C. The game writes mode `$88`, so
C's lower half drives the PAL and the upper half reads the result.
Stern 33A-105 and 33A-106 do not print equations for 6J. The scramble
program at `$007E` shifts three nibbles. Window `F09` must read back
`$F0`, then window `A49` must read back `$B0`. Any other window leaves
the last result, which is what the `$1D75` loop is waiting on when it
writes `09`.

## 6. Inputs

Active low, 8-way. Cocktail player 2 is unmapped.

IN0: bomb `$02`, fire `$08`, R/L `$10/$20`, coins `$40/$80`.
IN1: lives bits 1:0 (default 00 = 3), starts `$40/$80`.
IN2: U/D `$10/$40`, coinage bits 2:1, cabinet bit 3, protection bits 5/7.

## 7. Unsourced

- Cocktail player 2's stick is unmapped.
- M51516L input load taken as 100 kΩ. No Mitsubishi data sheet found.
- High score `$4200` (10 × 3 BCD) and the displayed score at `$40A8`
  have no battery. The page keeps them in IndexedDB for a user ROM.
  Reset HIGH SCORE deletes the save and resets the board. Source: Tunstall's
  Scramble ROM disassembly (seanriddle.com/scramble.asm). Cold start
  (`$0195`) writes ten `00 00 01` entries at `$4200` and `01` at `$40AA`;
  the insert routine is `$1359`/`$137F`. The restore waits for all 33 bytes
  to equal that image.

## ROM policy

Ships a from-scratch hwtest ROM. A user `scramble` set stays in
IndexedDB (`retroweb-scramble`). Size-checked. Native `play_test` skips
unless `roms/user/`, `SCRAMBLE_ROM`, or `~/Downloads/scramble.zip` holds
a dump.

## 8. Tests

- `make -C retroweb/scramble check`
- `make -C retroweb/scramble/web test` (Playwright on :8700)

## 10. Shared video and Konami sound

`retroweb/shared/galaxian/video.*` draws as the beam scans. H counts
from 176, where HSYNC clocks V. Sprite slots load a line buffer during
horizontal blank and only write where it still holds 0, so a
lower-numbered sprite wins. The first 16 pixels are clipped. Slots 0–2
fall before HSYNC and match the previous line. Shells and the missile
each have one counter a line, and the last match wins. Flip inverts the
counters ahead of the scroll adder. Stars are a 17-bit shift register,
two clocks a pixel. Galaxian's origin drifts one step a frame, and
stars-enable going high restarts the field at the beam.

Each colour gun is a TTL high (3.4 V) through its resistors into 470 Ω.
The PROM uses 1 kΩ / 470 Ω / 220 Ω on red and green and 470 Ω / 220 Ω
on blue. Stars use 150 Ω / 100 Ω through a 74366, a 0 bit driving the
resistor high. Frogger's river is 470 Ω of blue. Scramble's background
is 390 Ω of blue.

`ay8910.*` is the GI AY-3-8910. Tone half-period is 8 × TP clocks,
noise is 16 × NP, and each envelope step is 16 × EP. Level 15 is 1 V
and each step down is 3 dB. Level 0 is 0 V. Held shapes sit at 15 or 0
the way the data-sheet chart draws them. Shapes that repeat hold the
end level for one step at each turn.

`konami_sound.*` is the output stage on Stern 33A-105 and Frogger
834-0069. Each channel is 1 kΩ, then a CD4066 (470 Ω on) switching
0.22 µF and 0.047 µF, then 5.1 kΩ into an LM741. Feedback is 2.2 kΩ on
Scramble and 5.6 kΩ on Frogger. The swing stops 2 V short of the ±5 V
rails. Then 4.7 kΩ and 0.15 µF into the amp, whose input is taken as
100 kΩ. The latch takes AV6–AV11 for one AY and AV0–AV5 for the other.
A full-level channel is scaled to 0.5 peak to peak.

The NMI flip-flop sets at vblank and clears only when the enable latch
is written 0 (`$7001` Galaxian, `$B808` Frogger, `$6801` Scramble).
The sound CPU's INT flip-flop holds /INT until the acknowledge.
