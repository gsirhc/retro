# Space Invaders arcade board: design notes

Citation trail for the Midway Space Invaders (1978) emulator, PCB #739.
Primary source is MAME `src/mame/midw8080/mw8080bw.cpp`, `mw8080bw.h`,
`mw8080bw_v.cpp`, `mw8080bw_a.cpp` and `src/devices/machine/mb14241.cpp` at
commit `d404316732fb5970cc0170aefc69ef1a85e14fce`, whose header comments are
taken from the Midway schematics. The game's RAM use and code paths are
checked against the Computer Archeology disassembly
(computerarcheology.com/Arcade/SpaceInvaders/).

## 0. Status

Single 8080, 1-bit bitmap, MB14241 shifter, and discrete analog + SN76477
sound, covered by GoogleTest (`make check`) and a generated self-test ROM.
The front end ships the self-test. A real Midway `invaders` set is opt-in and
browser-local. Hub card, marquee tile and CI jobs (`invaders-test` /
`invaders-web-test`, both after `i8080-test`) are wired in.

## 1. Scope

MAME parent `invaders` only: four 2K ROMs at `$0000`/`$0800`/`$1000`/`$1800`
(`9316b-0869_m739h.h1` … `9316b-0854_m739e.e1`, or the older `invaders.h` …
`invaders.e`), or one 8K `program.bin`. Upright PCB only. Taito's `sitv`/`sicv`
and the cocktail PCB are not modelled. On the upright PCB the flip-screen line
(OUT 5 bit 5) is not connected and IN2 carries P1 controls, so there is no
cocktail stick to map.

## 2. Clocks (never sped up on the live page)

- Master 19.968 MHz. 8080 at /10 = **1.9968 MHz**. Pixel clock /4 = 4.992 MHz.
- 320 pixel clocks a line = **128 T-states**. 262 lines = **33,536 T-states a
  frame**, 59.54 Hz.
- The V counter steps when H overflows inside HBLANK, so each line starts at
  H=0 (MAME header, "Vertical sync chain").

## 3. CPU

`retroweb/shared/cpu/cpu_i8080.{h,cpp}`, the Altair's core, moved there so
both machines run one 8080. ISA coverage is the `I8080*` GoogleTest suites
plus 8080PRE, TST8080, CPUTEST and 8080EXM through `cpm/cpm_host`
(`make -C retroweb/shared/cpu i8080diag`).

## 4. Memory map (MAME `main_map`)

- A15 ignored (`global_mask(0x7fff)`).
- `$0000–$1FFF` ROM, writes dropped.
- `$2000–$3FFF` RAM, mirrored at `$6000–$7FFF`. `$2400–$3FFF` is the bitmap.
- `$4000–$5FFF` empty ROM socket, reads 0 as in MAME's zero-filled region.

## 5. I/O (MAME `invaders_state::io_map`, A0–A2 decoded)

Reads: 0 IN0, 1 IN1, 2 IN2, 3 shifter result, mirrored at 4–7.
Writes: 2 shift count, 3 sound 1, 4 shift data, 5 sound 2, 6 watchdog.

Inputs, upright PCB (MAME `INPUT_PORTS_START( invaders )`):

| Port | Bits |
|---|---|
| IN0 | 0 SW8, 1–2 SW6/SW7, 3 pulled high, 4–6 P1 fire/left/right, 7 SW5 |
| IN1 | 0 COIN **active-low**, 1 START2, 2 START1, 3 pulled high, 4–6 P1 fire/left/right |
| IN2 | 0–1 lives (3–6), 2 TILT n/c, 3 extra base at 1000 (0 = 1500), 4–6 P1 controls, 7 coin info off |

The disassembly's ISR at `$0020` reads `IN 01; RRCA; JC` as "coin
deposited", so with MAME's active-low COIN a credit registers when the switch
is pressed. SW5–SW8 are unused by this ROM and not on the DIP panel.

## 6. MB14241

`count = ~data & 7`, `data = data >> 8 | new << 7`, result `data >> count`
(MAME `mb14241.cpp`). Equivalent to the usual description: a 16-bit register
with the newest byte high, read as `reg >> (8 - n)`.

## 7. Interrupts

E3 is a D flip-flop clocked by `(!(64V | !128V) | VBLANK)`, so it fires at
V = 0x80 (line 96) and V = 0xDA with VBLANK (line 224). It latches the 8080's
INTE pin, which is what MAME's `interrupt_trigger` does (assert if
`m_int_enable`, else clear). INTA clears it. The vector is sampled at INTA as
`0xC7 | 64V << 4 | !64V << 3`: RST 1 (`$CF`) at line 96, RST 2 (`$D7`) at 224.
An acknowledge that slips past line 160 reads RST 2, as on the board.
`Machine::run_cycles` samples INT after each instruction, once the beam has
caught up.

## 8. Video

- The beam is emulated. Each VRAM byte loads the shift register at pixel
  `8k+4` of its line (MAME `screen_update_invaders`), at 2.5 pixels per
  T-state, so a write lands on screen this frame only if the beam has not
  passed it. Granularity is one instruction.
- The frame is latched at line 224 and shown upright (MAME ROT270): 224×260,
  the bottom four rows are the shift register flushing zeros after HBLANK
  (`MW8080BW_HPIXCOUNT = 256 + 4`).
- The tube is black and white. Color comes from the Midway upright's
  cellophane strips, multiplied over the canvas in `app.js` with MAME
  `invaders.lay`'s rectangles: red y 32–62, green y 184–240, and green y
  240–260 for x 16–134. The moon backdrop art is copyrighted and not drawn.

## 9. Watchdog

255 frames without OUT 6 (MAME `WATCHDOG_TIMER … 255 * MW8080BW_60HZ`). Only
the CPU is reset. RAM, the beam and the sound latches carry on.

## 10. Sound (`sound.cpp`)

MAME `invaders_audio_device` and the `invaders_discrete` netlist, module by
module at a 48 kHz step:

- OUT 3: bit 0 high runs the SN76477 (MAME `enable_w(BIT(~data,0))`, the
  chip's enable is active-low), 1 missile, 2 explosion, 3 invader hit, 4 bonus
  base, 5 amplifier enable.
- OUT 5: bits 0–3 fleet thump data, 4 saucer hit, 5 flip screen (n/c upright).
- Ported literally: `midway_lfsr` noise at 7515 Hz, `dss_op_amp_osc`
  (OSCILLATOR_1, VCO_1, VCO_3, Norton), `dst_op_amp`, `dst_op_amp_1sht`,
  `dsd_555_astbl` with the R126–R130 comp adder, RC/CR filters,
  `dss_squarewfix`, `dst_mixer` with C11 and C45, SN76477 SLF/VCO/output.
- Reduced to what the netlist uses: `dst_tvca_op_amp` (f2 = TRG0 only), the
  SN76477 noise path (mixer mode 0 is VCO only), and its one-shot/attack with
  zero C and R (the A/D voltage sits at 4.44 V once enabled).
- MAME's `DISCRETE_OUTPUT` gain 2500 is read as /32768 into the float stream.
  Final mix is 0.5 discrete + 0.5 SN76477, as MAME routes both.
- MAME notes the missile VCO_3 does not fully match the board. It is quiet
  here as in MAME.

## 11. HIGH SCORE

`$20F4–$20F5`, BCD, low byte first (disassembly `HiScorL`/`HiScorM`).
`init` at `$18D4` copies `$1B00–$1BFF` over `$2000–$20FF` and draws the
status line at `$18DC`, so the restore waits 30 frames for that copy to
leave zeros. The game redraws HI-SCORE only at coin-up (`$07C8`) and game
over (`$1695`), so a restored score is live in RAM straight away and on
screen from the next coin-up.

## 12. Self-test ROM (`roms/hwtest/gen_hwtest.py`)

Original 8080 code assembled in Python. Signature `TST1` at `$2000`, an
MB14241 check (result at `$2005`, pass flag at `$2004`), a 16-pixel
crosshatch with the four fleet notes, a full-white field with the saucer
(shows the gels), then the help screen in this repo's own 8×8 font, rotated
for ROT270. RST 1 counts at `$2009`, RST 2 at `$2008`; the main loop feeds the
watchdog.
