# Lunar Rescue arcade board: design notes

Citation trail for the Taito Lunar Rescue (1979) emulator. Primary sources:

- Taito PV game board schematic PVR00001, dated 7/19/79 (`tcsi-sp.pdf`,
  the Taito Space Invaders Part II schematic set). Its sound section is the
  board Lunar Rescue's OUT 3 / OUT 5 bits drive.
- Lunar Rescue service instructions (Taito America), for the controls and
  the operator settings.
- The shared Midway 8080 board, in `invaders/INVADERS_REVIEW.md`.

The colour PROM row is the vertical counter. The red screen and the gun
gate are on PVR00001 sheet 5, the PROM bit order is Unsourced (section 4).

The high-score blocks are the `lrescue` ROM, below.

## 0. Status

Shared 8080 board, colour PROM, red-screen latch and the Taito PV discrete
+ SN76477 sound, covered by GoogleTest (`make check`) and a generated
self-test ROM. The front end ships the self-test. A real Taito `lrescue` set
is opt-in and browser-local. Hub card, marquee tile and CI jobs
(`lunarrescue-test` / `lunarrescue-web-test`, both after `i8080-test`) are
wired in. `Play.*` runs when `~/images/arcade/lrescue.zip` is present.

## 1. Scope

Taito set only: `lrescue.1`–`lrescue.4` at `$0000`–`$1FFF`,
`lrescue.5`–`lrescue.6` at `$4000`–`$4FFF`, and the 1K colour PROM
`7643-1.cpu`. Upright cabinet. The bootlegs (`mlander`, `lrescuem`, …) and
the cocktail flip are not modelled.

## 2. Shared board

Everything below the cabinet layer is `retroweb/shared/mw8080/board.{h,cpp}`,
shared with Space Invaders: 1.9968 MHz 8080, 128 T-states a line, 262 lines,
INT latched at lines 96 and 224 with the 64V vector, shifter, the beam
fetching VRAM at pixel `8k+4`. See `invaders/INVADERS_REVIEW.md` §2–§8. The
second ROM bank at `$4000` is the socket Invaders leaves empty.

PVR00001 sheet 5 shows a 7442 (IC13) PORT6 output wired to the CL pins of
two 74161s (IC2, IC8) that count the 60 Hz clock, so OUT 6 looks like the
same watchdog clear Midway has, not an unused port. The cabinet still runs
with `watchdog_enabled = false`. That is Unsourced against this sheet and
needs a decision, since the self-test ROM never writes port 6.

## 3. Inputs

| Port | Bits |
|---|---|
| IN0 | `0xFF`. Sheet 5 input muxes are four 74153s: IC4 (bits 0-1), IC9 (2-3), IC5 (4-5), IC10 (6-7), 1Y low bit, 2Y high bit. Port 0 is pins 6 and 10. Bits 2, 4, 6, 7 are 1K to +5 V, bit 5 (IC5 pin 10) is not connected |
| IN1 | 0 COIN **active-low**, 1 START2, 2 START1, 3 high, 4–6 P1 fire/left/right, 7 floating high |
| IN2 | 0–1 rescue ships (3–6), 2 TILT, 3 and 7 unused DIPs, 4–6 P1 controls on the upright |

Unsourced input bits: IC4 pin 6 (IN0 bit 0) runs to a bus not yet
traced. IC4 pin 10 (IN0 bit 1) and IC9 pins 10 and 11 (IN0 bit 3, IN1 bit 3)
sit on nets drawn with both a 1K to +5 V and a ground symbol, so the idle
level is unresolved and `0xFF` / `0x89` keep them high. IN1 bit 7 (IC10
pin 5 side) and bit 3 are not tested by `lrescue.1`-`.4` at the `IN 1` reads
(`$0020`, `$00A0`, `$018F`, `$05EE`), and the ROM has no `IN 0`.

The manual lists the ship count as the only operator setting and calls SW1:3–8
factory adjustments, so the DIP panel has Lives only. The one red button is
both fire and the retro rocket.

## 4. Video

- Foreground pen is the colour PROM, one colour per upright 8×8 cell,
  black where the bitmap is off. The row is `vcounter(line) >> 3`
  (line 0 is V `$20`, so the first band is row 4) and the column is the
  same cell as the bitmap. The colour is the low 3 bits.
- Gun gate, PVR00001 sheet 5 (bottom left, "used on color display only"):
  IC16's three OR gates and the 7416 open-collector drivers combine VIDEO (from the
  shift register) with the RED, GREEN and BLUE colour inputs and BOMB.
  Red = VIDEO and (RED or BOMB). Green = VIDEO and GREEN and not BOMB. Blue =
  VIDEO and BLUE and not BOMB (IC23 and IC29 NANDs on the inverted BOMB).
  Where the bitmap is off every gun is off.
- BOMB is the Q output of the third flip-flop of the OUT 3 latch (IC15,
  pin 7, the same net as SX2 before its inverter). So OUT 3 bit 2 turns
  every lit pixel red, whatever the PROM says. It is latched per line at the
  start of each line (`Board::beam_tag`), so a write mid-frame turns only
  the lines below the beam red.
- The sheet takes RED, GREEN and BLUE in on off-sheet pads 4-Q, 4-W and 4-Y
  and does not draw the colour PROM. Unsourced: which PROM data bit goes to
  which pad. The code uses bit 0 red, bit 1 blue, bit 2 green, and PROM bit 3
  goes nowhere.
- The shared board keeps the shift-register timing and the four blank rows.
- The second PROM half is unused, so only the first 1K is loaded.

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

Not on the sheet:

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

Derived by disassembling the Taito `lrescue` program ROM (`lrescue.1` to
`lrescue.4` at $0000-$1FFF, `lrescue.5` and `.6` at $4000; a local dump, not
in the repo).

- Init: reset jumps to $164B. It copies 256 bytes from ROM $1AA3 to $2000
  (`CALL $0B04`, B = 0), then $C0 bytes from $1BA3 to $2100 and $2200. The
  first page is the game's variable block, so every variable starts as
  ROM $1AA3 + n.
- Table: one entry. The holder's name is ten glyph codes at $20CF, its length
  is $20DB, and the score is two BCD bytes, low byte first, at $20F4-$20F5.
  The image holds `1B 1B 13 00 08 13 0E 1B 1B 1B` for the name, `00` for the
  length and `00 05` for the score (0500). Letters run from code 0 and digits
  start at $1C (`ADI $1C` at $0587), so that reads blank, blank, T, A, I, T,
  O and three blanks, and `$1B` is the blank. $20F6-$20F7 (`1D 2F`) is the
  screen pointer for the score and is not saved. The name position $20CD is
  recomputed on every draw.
- Score-line draw: $0ABD (every attract and play screen) draws the score
  through $0AB7, then calls $0920. With length 0 it sets the position to
  $2D1E and stores $0A in $20DB, so the first draw turns the factory length
  from 0 into 10. Otherwise it centres the name from the length. $088D draws
  $20DB glyphs from $20CF.
- Insert: $02B6 (from $04B0 and the high bank at $4AD0, $4AF1). $058C picks
  the player's score ($20F8 for P1, $20FC for P2, by bit 0 of $20C6). The code
  compares the high bytes against $20F5, then the low bytes against $20F4.
  A larger score is copied into $20F4-$20F5 and $4700 redraws it and stores
  the player in $20E3. $04D3 then runs name entry ($470A, $0657).
- Name entry: $0657 copies 26 bytes from $08C2 to $20CB. That sets the name to
  ten `1B`, the length to 0 and the entry flag $20DC to 1. $0842 stores a
  letter at $20CF + length and increments the length, $0810 deletes one. When
  entry ends $0878 clears $20DC, and $0920 turns a length of 0 into 10.
- Saved: $20CF x 10, $20DB, $20F4 x 2. The on-screen score is drawn from
  $20F4, with no second copy.
- Ready and factory: the app builds the image from the loaded program (ROM
  $1AA3 + offset, with a zero length read as 10). Restore waits until the
  length is 10 and RAM equals it, so the image copy at reset and POST junk
  never pass. A save is not written while RAM equals it, or equals it with
  length 0.

## 7. Self-test ROM (`roms/hwtest/gen_hwtest.py`)

Original 8080 code and an original colour PROM, assembled in Python.
Signature `TST1` at `$2000`. Checks the shifter (pass flag `$2004`, result
`$2005`) and the second ROM bank (`BNK2` at `$4000` plus a routine run from
there; pass flag `$200A`). Then a 16-pixel crosshatch with the four step
notes, a full-white field that shows the PROM's eight bands (white, green,
yellow, cyan, blue, magenta, red, white) with the SN76477 on, 30 frames of
OUT 3 bit 2 red screen, and the help screen in this repo's 8×8 font, its rows
coloured by the same bands. `ROM 5-6 BAD` or `SHIFTER BAD` replaces
`SELF TEST OK` on a failure.
