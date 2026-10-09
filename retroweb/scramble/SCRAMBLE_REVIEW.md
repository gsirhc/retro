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
| `$6801` | NMI enable (D0 low also clears the held NMI) |
| `$6802` | coin counter |
| `$6803` | scramble background enable |
| `$6804` | Galaxian stars enable |
| `$6806` / `$6807` | flip X / Y |
| `$7000` | watchdog **read** (returns `$FF`) |
| `$8000–$FFFF` | PPI0 if A8, PPI1 if A9 (`theend_ppi8255_*`) |

Sound CPU: ROM `$0000–$1FFF` (6K populated), RAM `$8000–$83FF` (mirrored).
AY I/O is bit-decoded: bits 4–5 = AY2 (addr/data), bits 6–7 = AY1
(addr/data). AY1 port A is the command latch; port B is the **generic**
Konami sound-board timer (no Frogger 3↔5 swap). Tone f = fclock/(16·TP);
envelope step is 16·EP clocks. Filter latch writes at `$9000–$9FFF`
(mirror `$6000`) set the 4066 switches from the address lines (§10).

## 5. Video

Galaxian tilemap, 32×32 of 8×8, 2bpp. No nibble-swap, no river split, no
D0↔D1, full PROM blue bits. Galaxian **starfield** (17-bit LFSR) plus a
555 blink (`Ra=100k, Rb=10k, C=10µF`). Scramble background latch at
`$6803` paints a 390 Ω blue. Eight shells in object RAM at `$60` (yellow,
two pixels, one shell and one missile per line). Stars use `y * 512` with
no frame drift. See §10.

## 6. Protection PAL at 6J

Nibble in / nibble out on PPI1 port C. The game sets PPI1 to mode `$88`
(its own `LD A,$88; LD ($8203),A`), so C's lower half drives the PAL and
its upper half reads the result off the pins. Exact PAL equations are not
published. Scramble uses op `$9` (increment); ops `$6`/`$F` and `$A`/`$B`
are the bootleg and The End sequences MAME `theend_protection_w` lists.
IN2 bits 5 and 7 feed the “alt” bits derived from the high bit of that
nibble.

## 7. Inputs / DIPs

Active-low, 8-way. Cocktail P2 unmapped (same labelled skip as Frogger).

IN0: bomb `$02`, fire `$08`, R/L `$10/$20`, coins `$40/$80`.
IN1: lives DIP bits 1:0 (default 00 = 3), starts `$40/$80`.
IN2: U/D `$10/$40`, coinage bits 2:1, cabinet bit 3, protection bits 5/7.

## 8. User ROM

Optional. `roms/user/`, `SCRAMBLE_ROM`, or `~/Downloads/scramble.zip`.
Size-check only, MAME `scramble` chip names. IndexedDB `retroweb-scramble`.
CI never has a Konami dump, so playthrough tests skip there.

## 9. Known simplifications

- Cocktail P2 stick unmapped.
- PAL 6J equations (parity K3) are inferred from observed sequences; MAME
  also says "Logic is not exactly known". Needs a PAL dump.
- **HIGH SCORE RAM is volatile on the real PCB.** There is no battery.
  `$4200` (10 × 3 BCD) plus displayed HI at `$40A8` die on power-off. The
  Konami parent set has no initials. This page always persists those bytes
  in IndexedDB for a user ROM — a labelled departure. **Reset HIGH SCORE**
  deletes the save and `machine.reset()`s the board. Covered by
  `web/tests/hiscore.spec.ts`.

## 10. Parity fixes (2026-10-09)

This section covers the Galaxian-family video and Konami sound shared by
Scramble, Frogger and Galaxian. Items V1-V7, K1, K2, B1-B3, X3 from
`shared/cpu/Z80_ARCADE_PARITY.md`.

### Shared video (`shared/galaxian/video.cpp`)

*Fact:* the board draws as the beam scans. Sprites load a 256-pixel line
buffer during HBLANK that only accepts writes where it still holds 0, so
lower-numbered sprites win; the first 16 pixels (17 with the +1 offset,
mirrored when flipped) are hard-clipped; the first three sprites match
one line late. Shells and the missile use one counter each per line,
last match wins. Stars come from a 17-bit RNG clocked twice per pixel.
*Why:* the old renderer drew the frame at once from final RAM, reversed
sprite priority, drew every bullet match, and ignored flip for sprites,
bullets and stars. *What:* the main Z80's `Bus::tick` drives
`Video::advance`, which paints spans as the beam crosses them and runs
`hblank_setup` for the next line. Flip inverts the H/V counters ahead of
the scroll adder. Galaxian's star origin drifts one RNG step a frame (+1
when flipped X) and `$7004` rising restarts it at the beam. Shells and
the missile are OR'd into the RGB: Galaxian/Frogger four pixels, shells
white and missile yellow; Scramble two yellow pixels. The palette uses
MAME's resnet weights with the 470 Ω pulldown, scaled to 224, with star
levels 194/214/255. *Source:* MAME `galaxian_v.cpp` (`sprites_draw`,
`bullets_draw`, `stars_update_origin`, `galaxian_palette`), which quotes
the schematics. Tests: `Video.LowerNumberedSpriteWins`,
`Video.FirstThreeSpritesMatchAgainstLineMinusOne`,
`Video.SpriteLineBufferClipsFirstSixteenPixels`,
`Video.GalaxianStarFieldDriftsOneStepPerFrame`,
`Video.StarsEnableRestartsTheFieldAtTheBeam`,
`Video.OneShellPerLineLastMatchWins`, `Video.GalaxianMissileIsFourYellowPixels`,
`Video.ShellsOrIntoTheTileColour`, `Video.FlipXMirrorsTheTilemap`,
`Video.FlipYMirrorsTheTilemap`, `Video.MidFrameWritesTakeEffectAtTheBeam`,
`Video.VblankEdgeFiresOncePerFrameAtLine240`, `Video.ScrambleShellsAreTwoYellowPixels`,
`Video.FroggerShellsAreFourWhitePixels`.

### Held vblank NMI

The NMI flip-flop is set at vblank and cleared only by writing 0 to the
enable latch (`$7001` Galaxian, `$B808` Frogger, `$6801` Scramble). Games
clear and re-arm it inside the handler; one that doesn't gets one NMI.
The three self-test ROMs now do it like the games. MAME `irq_enable_w`.

### Konami sound network (`shared/galaxian/konami_sound.cpp`)

*Fact:* each AY output is an NMOS source follower. MAME fits it from die
measurements as a Thevenin resistance to 5 V per DAC level. Each channel
then runs through RI 1 kΩ to a node with two caps (0.22 µF, 0.047 µF)
switched in by a CD4066 (about 270 Ω on), then RO 5 kΩ into a uA741
summer (2.2 kΩ feedback). That feeds the 4.7 kΩ / 200 Ω volume divider,
then 0.15 µF into the M51516L amp (about 100 kΩ). The 4066s are
controlled by a latch whose data is the CPU's address lines: AV6-AV11
for AY 3D, AV0-AV5 for 3C, low bit 0.22 µF. *Why:* the old output was a
dry 3 dB ladder mix with every filter write ignored. *What:* `KonamiSound`
solves each channel node by backward Euler every 8 AY clocks, sums into
the op-amp (clipped at about ±4 V), applies the coupling, and box-averages
to the host rate. The output is scaled 1/0.05 like MAME, so Frogger's
4x page gain is gone. *Source:* MAME `nl_konami.cpp` (Couriersud),
`ay8910.cpp` `build_mosfet_resistor_table`, `galaxian.cpp`
`konami_sound_filter_w`. Tests: `Ay8910.MosfetOutputResistanceFallsWithLevel`,
`Ay8910.OutputLevelIsZeroWhileTheMixerGatesTheChannel`,
`KonamiSound.SwitchedCapsDampTheTone`, `KonamiSound.OutputIsAcCoupled`,
`KonamiSound.MuteSilencesTheOutput`,
`Machine.FilterLatchDecodesAddressLinesAt6000` (Frogger),
`Machine.FilterLatchSplitsAddressLinesBetweenAys` (Scramble).

### AY envelope hold

The held level for continue+hold shapes was inverted: shapes 11 and 13
hold at 15, shapes 9 and 15 at 0 (GI datasheet envelope chart; MAME
`set_shape`). `Ay8910.HeldEnvelopeLevelsMatchTheDatasheet`.

### Sound CPU INT

Control bit 3 falling sets a flip-flop that holds the sound Z80's /INT
until the acknowledge clears it (MAME `konami_sound_control_w`
HOLD_LINE). It used to be a one-shot with a retry flag.
`Machine.SoundIntHoldsUntilTheCpuAcknowledges` (Frogger).

### Board items

- **Watchdog keeps RAM (B1)**, `Machine.WatchdogResetKeepsRam`.
- **Coin counter (B3).** `$6802` D0 (MAME `coin_count_0_w`).
  `Machine.CoinCounterCountsRisingEdges`.

The real `scramble` set boots, takes a coin, starts and fires
(`play_test` with `SCRAMBLE_ROM`), and frames dumped from it look right.

### Found closing the coverage gaps

- **8255 bit set/reset.** A control write with D7 low sets or clears one
  port C line (Intel 8255A datasheet) and re-sends the port, as MAME
  `output_pc` does. Ours ignored BSR, so a BSR write to the PAL went
  nowhere. `I8255.BitSetResetDrivesOnePortCLine`,
  `Machine.Pal6JSeesBitSetResetWrites`.
- **No result in the port C latch.** `pal6j_write` also copied the result
  into PPI1's output latch, which a BSR would then have sent back to the
  PAL. In mode `$88` the result comes in on the input pins, so the copy is
  gone. A port C read in that mode gives the result's high nibble and the
  last nibble written. `Machine.Pal6JOp9IncrementsNibble`,
  `Machine.Pal6JOtherOpsFollowMame`.
- **Repeating AY envelopes.** Shapes 8, 10, 12 and 14 had no test. They
  now match the datasheet waveforms step by step, with the end level held
  for one step at each turn of the triangle, as MAME's AY does.
  `Ay8910.RepeatingEnvelopeShapesMatchTheDatasheet`.
- `play_test` also looks in `~/images/arcade/scramble.zip`.
