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

MAME plays samples. The real board is the Taito PV game board's discrete
section (PVR00001), the same topology as Midway's `invaders_discrete` with
Taito's part values. `shared/mw8080/sound.cpp` runs it with
`SoundBoard::TaitoPv`.

Port map (latch SX0–SX10 on the schematic; MAME's sample names in brackets):

- OUT 3: 0 SN76477 enable, active-low pin [thrust], 1 trigger one-shot
  [shot], 2 flash / explosion [death] and screen red, 3 target hit [alien
  hit], 4 extend play [bonus ship], 5 amplifier enable.
- OUT 5: 0–3 step ladder into the step 555 [footsteps, bonus count, and the
  "bitstream" tunes, which are bit 3 toggling the 555], 4 UFO hit [shooting
  star], 5 flip (n/c upright).

Taito deltas from Midway, read off PVR00001:

| Part | Midway | Taito |
|---|---|---|
| Trigger one-shot C12 | 1 µF | 0.22 µF |
| Trigger VCO cap | 330 pF | 300 pF |
| Target-hit oscillator cap | 0.1 µF | 0.22 µF |
| Flash output R84 | 5.6K | 560 Ω |
| Extend play gate | 480 Hz tap | 960 Hz tap |
| Mixer | resistor summing into one op amp | 1 µF → B-50K trimmer VR1–VR7 → 47K → 1 µF → Norton amp |

Output series resistances into the trimmers: UFO 47K, trigger 0.001 µF +
150K, flash 2K, target hit 47K, extend play 220K, step 68K, UFO hit 47K.
The summing amp's AC feedback is 100K in parallel with VR8 (50K, in series
with 1 µF); SX5 mutes it through a diode. Then 0.1 µF / 47K into the MB3712.

Assumptions, each a single constant in `sound.cpp`:

- Every trimmer sits at mid travel (`kTaitoWiper`). Operators set these by
  ear, so the real balance varies by cabinet. At mid travel the UFO / thrust
  is quieter than the hits, as the series values imply.
- `kTaitoGain`: 1 V at the summing amp output is full scale. The loudest
  single sound (target hit) peaks near 0.55.
- The flash capacitor C24 is not legible on the scan. Midway's 2.2 µF is
  kept.
- MAME's note that Taito boards use 2.2M for R13 is not on this sheet, which
  shows 3.3M. 3.3M is used.
- The second 1 µF after each 47K is a ~3 Hz high-pass and is not modelled.

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
