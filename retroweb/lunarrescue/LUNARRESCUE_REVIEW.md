# Lunar Rescue arcade board: design notes

Citation trail for the Taito Lunar Rescue (1979) emulator. Primary sources:

- MAME `src/mame/taito/8080bw.cpp`, `8080bw_a.cpp`, `8080bw_v.cpp` at commit
  `d404316732fb5970cc0170aefc69ef1a85e14fce` (`lrescue` driver, inputs,
  video, sample-based sound handlers).
- Taito PV game board schematic PVR00001, dated 7/19/79 (`tcsi-sp.pdf`,
  the Taito Space Invaders Part II schematic set). Its sound section is the
  board Lunar Rescue's OUT 3 / OUT 5 bits drive.
- Lunar Rescue service instructions (Taito America), for the controls and
  the operator settings.
- MAME `plugins/hiscore/hiscore.dat`, `lrescue` entry, for HIGH SCORE RAM.

## 0. Status

Shared 8080 board, colour PROM, red-screen latch and the Taito PV discrete
+ SN76477 sound, covered by GoogleTest (`make check`) and a generated
self-test ROM. The front end ships the self-test. A real Taito `lrescue` set
is opt-in and browser-local. Hub card, marquee tile and CI jobs
(`lunarrescue-test` / `lunarrescue-web-test`, both after `i8080-test`) are
wired in. No real dump was available while this was built, so the `Play.*`
tests skip and the HIGH SCORE addresses come from hiscore.dat only.

## 1. Scope

MAME parent `lrescue` only: `lrescue.1`–`lrescue.4` at `$0000`–`$1FFF`,
`lrescue.5`–`lrescue.6` at `$4000`–`$4FFF`, and the 1K colour PROM
`7643-1.cpu`. Upright cabinet. The bootlegs (`mlander`, `lrescuem`, …) and
the cocktail flip are not modelled.

## 2. Shared board

Everything below the cabinet layer is `retroweb/shared/mw8080/board.{h,cpp}`,
shared with Space Invaders: 1.9968 MHz 8080, 128 T-states a line, 262 lines,
INT latched at lines 96 and 224 with the 64V vector, MB14241, the beam
fetching VRAM at pixel `8k+4`. See `invaders/INVADERS_REVIEW.md` §2–§8. The
second ROM bank at `$4000` is the socket Invaders leaves empty.

MAME `lrescue_io_map` maps port 6 to `nopw` ("noise? LED?"), so the cabinet
runs with `watchdog_enabled = false`.

## 3. Inputs (MAME `sicv_base`, `lrescue`)

| Port | Bits |
|---|---|
| IN0 | all tied high through 1K, bit 5 floating: `0xFF` |
| IN1 | 0 COIN **active-low**, 1 START2, 2 START1, 3 high, 4–6 P1 fire/left/right, 7 floating high |
| IN2 | 0–1 rescue ships (3–6), 2 TILT, 3 and 7 unused DIPs, 4–6 P1 controls on the upright |

The manual lists the ship count as the only operator setting and calls SW1:3–8
factory adjustments, so the DIP panel has Lives only. The one red button is
both fire and the retro rocket.

## 4. Video

- MAME `screen_update_invadpt2`: foreground pen is
  `prom[(offs >> 8 << 5) | (offs & 0x1F)] & 7`, black background. With
  `offs = V * 32 + k` and V = line + 0x20, that is
  `prom[((line >> 3) + 4) * 32 + k]`, one colour per upright 8×8 cell.
- Pens are `RBG_3BIT`: bit 0 red, bit 1 blue, bit 2 green.
- OUT 3 bit 2 sets `m_screen_red`, forcing every lit pixel to pen 1. Here it
  is latched per line at the start of each line (`Board::beam_tag`), so a
  write mid-frame turns only the lines below the beam red.
- MAME's `invadpt2` draws straight from RAM and pads the four flushed
  columns by hand. That is an emulator shortcut. The shared board keeps the
  real shift-register timing and the four blank rows.
- `m_color_map` (the second PROM half) is never written by `lrescue`, so
  only the first 1K is used.

## 5. Sound

The real board is the Taito PV game board's sound section (schematic
PVR00001, 7/19/79, sheet 5 of `tcsi-sp.pdf`). It is the Midway 739 sound
circuit with Taito part values and a trimmer mixer, so
`shared/mw8080/sound.cpp` runs the same model with `SoundBoard::TaitoPv`
(see `invaders/INVADERS_REVIEW.md` §10 for the circuit walk-through).

Port map (latch SX0–SX10 on the schematic):

- OUT 3: 0 SN76477 (IC32) enable through the IC20 inverter, 1 TRIGGER
  SOUND, 2 FLASH (also the red screen), 3 TARGET HIT, 4 EXTEND PLAY,
  5 amplifier enable.
- OUT 5: 0–3 STEP ladder into the IC17 556, 4 UFO HIT, 5 flip (n/c
  upright).

Taito deltas, read off PVR00001:

| Block | Midway 739 | Taito PV |
|---|---|---|
| Trigger one-shot cap | C12 1 µF | 0.22 µF (about 75 ms instead of 0.33 s) |
| Trigger oscillator cap | C10 330 pF | 300 pF |
| Trigger / flash / target / UFO-hit pull-ups | 1K to +5 V | 1K to +12 V |
| Target-hit sawtooth cap | C16 0.1 µF | 0.22 µF (about 5.4 Hz) |
| Flash ladder first resistor | R84 5.6K | 560 Ω |
| VCO slope switch | 4016 | 2SC872 with 33K base resistor |
| Extend-play tone | 480 Hz line | 960 Hz line |
| Outputs | R125 47K, R28 150K, R86 2K, R59 200K, R136 150K, R134 10K, R78 | 47K, 0.001 µF + 150K, 2K, 47K, 220K, 68K, 47K |
| Mixer | straight into the summing amp | 1 µF → VR1–VR7 → 47K → 1 µF → IC33 |

IC33 is the same summing stage as Midway's L2-3: 100K feedback, VR8 (50K)
plus 1 µF, 220K bias from a 2K/10 µF filtered +12 V, SX5 muting it through a
diode. Its swing about the bias point is the browser sample. IC34 MB3712
follows it.

Sources: PVR00001; the Midway 739 logic schematic for the values PVR00001
does not show; TI SN76477 data sheet and application guide; TI LM3900 data
sheet and National AN-72; TI NE555 data sheet; TI CD4006B data sheet.

Assumptions:

- VR1–VR7 have no value on the sheet: 50K like VR8, every wiper at mid
  travel. Each source then reaches the bus through about 41K and a 0.35
  current split, so the board is quieter than the Midway one. Operators set
  these by ear.
- VR8 at mid travel.
- SN76477 pins 11, 17, 18 and 20 are illegible on the scan; Midway's
  R123 150K, C36 0.047 µF, R121 39K and R120 120K are used. Pin 21's 1 µF,
  pin 10's 100K and pin 12's 10K are legible and match.
- The flash one-shot capacitor is illegible; Midway's C24 2.2 µF is used.
- No resistor is drawn between NOISE and IC27 pin 11; Midway's R79 680K is
  used, since a direct CMOS drive would hold the flash on regardless of SX2.
- The trigger noise network and the 4006 register follow Midway's wiring;
  the visible values (0.1 µF, 1M, 330K, 1.5M, 56K, 560K, 0.001 µF) match.
- The UFO / thrust SN76477 comes out very quiet at mid trimmer (about 0.003
  peak), following from equation 8 of the SN76477 data sheet and the
  trimmer split.

## 6. HIGH SCORE

MAME hiscore.dat `lrescue`: `$20DB` (1 byte, `$0A`), `$20CF` (10 bytes,
`$1B`), `$20F4` (2 bytes, `00 05`). The plugin waits for those factory bytes
before restoring, and `app.js` uses the same gate after 30 frames. Without a
dump the meaning of `$20CF` and `$20DB` is unconfirmed; all three blocks are
saved because that is what the plugin saves.

## 7. Self-test ROM (`roms/hwtest/gen_hwtest.py`)

Original 8080 code and an original colour PROM, assembled in Python.
Signature `TST1` at `$2000`. Checks the MB14241 (pass flag `$2004`, result
`$2005`) and the second ROM bank (`BNK2` at `$4000` plus a routine run from
there; pass flag `$200A`). Then a 16-pixel crosshatch with the four step
notes, a full-white field that shows the PROM's eight bands (white, green,
yellow, cyan, blue, magenta, red, white) with the SN76477 on, 30 frames of
OUT 3 bit 2 red screen, and the help screen in this repo's 8×8 font, its rows
coloured by the same bands. `ROM 5-6 BAD` or `SHIFTER BAD` replaces
`SELF TEST OK` on a failure.
