# Pac-Man arcade board — design notes and open questions

This document plays the same role as `retroweb/altair8800/ALTAIR_REVIEW.md`,
`retroweb/assembler6502/CGOAC6502_REVIEW.md`, and
`retroweb/ibmpc-at/IBM_PCAT_REVIEW.md`: a citation trail for the decisions
behind the emulator's behavior, and an honest list of what isn't built yet.

## 0. Status: v1 of the Midway Pac-Man board is done

The Z80 core (`retroweb/shared/cpu/cpu_z80.{h,cpp}`), video (`video.{h,cpp}`), Namco WSG
(`wsg.{h,cpp}`), and the board-level `machine.{h,cpp}` are implemented and
covered by GoogleTest (`make check`). The generated hardware self-test ROM
(`roms/hwtest/gen_hwtest.py`) exercises the CPU, tilemap, sprites, WSG,
watchdog, and IRQ path end to end against real memory-mapped addresses.
The Emscripten wrapper (`web/wasm_machine.cpp`) and canvas front end
(`web/index.html`, `web/app.js`) ship on the live page: cabinet bezel,
coin door, keyboard/joystick, theme chrome, and a size-checked ROM-set
loader (failed loads open a shared `.site-dialog`). Landing-page card and
CI jobs (`pacman-test` / `pacman-web-test`) are wired in
`retroweb/index.html` / `.github/workflows/deploy-emulator.yml`.

Playwright (`web/tests/`, `web/package.json`, `playwright.config.ts`)
covers boot of the self-test ROM (`smoke.spec.ts`, `help.spec.ts`),
keyboard/coin-door input including numpad 5/1/2 (`keyboard.spec.ts`,
`coindoor.spec.ts`), mute (`mute.spec.ts`), theme/fullscreen/focus-hint/
footer/home chrome, the ROM loader (loose chips, a `.zip` upload, and
the rejection dialog — `rom.spec.ts`), HIGH SCORE persistence
(`hiscore.spec.ts`), every DIP-switch control (`dip.spec.ts`; see
§7), and the Ms. Pac-Man query-string cabinet (`mspacman.spec.ts`, including
a skippable upload of a local `mspacman.zip`).
It never ships or fetches Namco's program. Two skippable native
playthroughs in `tests/play_test.cpp` coin in, start, and eat a pellet
when a local dump is present; CI skips both. Pac-Man is
`Machine.UserRomInsertsCoinStartsAndEatsAPellet` (`pacman`/`puckman`).
Ms. Pac-Man is `Machine.UserMsPacmanRomInsertsCoinStartsAndEatsAPellet`
(needs U5/U6/U7). See §8.

Coin-counter solenoids and the cocktail P2 stick stay out of scope
(see §9).

## 1. Scope: Midway Pac-Man (1980) PCB, plus the GCC Ms. Pac-Man aux board

The original Midway/Namco Pac-Man board is the default (`/pacman/`).
Ms. Pac-Man is the same page with `?game=mspacman`: the GCC conversion
kit's aux board in the Z80 socket, not a second machine. The landing
page has two cards to that one URL. Cocktail-cabinet player-2 joystick
and a distinct painted Ms. Pac-Man bezel stay out of scope (the home-page
card is an original 286×128 Courier New starfield tile from
`retroweb/shared/marquee.py`, not Midway art).

The aux board relocates the same Z80 (not a second CPU), adds U5 (2716)
and U6/U7 (2532), and a PAL/HAL overlay. Address and data lines on those
three ROMs are scrambled (US patent 4,525,599). When the dump-protection
latch is set (any access to `$3FF8–$3FFF`), `$3000–$3FFF` and
`$8000–$97FF` come from the aux ROMs and forty 8-byte windows in
`$0000–$2FFF` overlay decrypted `$8000–$81EF`. Latch-clear traps
(`$0038`, `$03B0`, `$1600`, `$2120`, `$3FF0`, `$8000`, `$97F0`) restore
the original Pac-Man map. The latch powers on clear; Pac-Man's IM 2
vector table sits in `$3FF8–$3FFF`, so the first vblank IRQ both enables
Ms. Pac-Man and reads U7's vector. Z80 RESET (watchdog) does not clear
the PAL. The patent describes the scheme (PROM-mapped address and data
swaps, trap detection on address values) but gives no line orders, so
the wiring and the forty windows were recovered from the U5/U6/U7 dumps
themselves, with the Pac-Man program chips as the reference for Z80 code:

- **Data pins.** A byte histogram is unchanged by any address swap, so
  each of the 8! data orders was scored by how closely the aux bytes'
  histogram matched the Pac-Man program's. Z80 D0-D7 come from EPROM
  D1, D2, D3, D6, D7, D5, D4, D0 (`kAuxDataPin`).
- **Address pins, low bits.** With data fixed, the EPROM line carrying
  Z80 A<k> was chosen, A0 first, by the likelihood of the byte pairs
  that become adjacent (Pac-Man byte-bigram model). U6 and U7 agree:
  A0-A2 straight, then A3 to pin 10, A4-A6 to pins 3-5, A7 to 9, A8 to
  6, A9 to 8, A10 to 7, A11 straight (`kU6U7AddrPin`). Swapping any two
  of A3-A11 lowers the share of JP/CALL targets that land on decoded
  instruction starts (86 of 87 as wired).
- **U5.** A0-A5 come out of the same bigram search (A3 to pin 4, A4 to
  3, A5 to 8). U5 splits into 62 eight-byte chunks plus a pointer table:
  40 chunks are near-copies of 8-byte-aligned Pac-Man code, 22 are blank
  `$FF`. All 40 have physical pins 6 and 7 low, so A9/A10 are those
  pins. The table at `$81F0` lists eight cutscene scripts; with A9 on
  pin 7 and A10 on pin 6 every entry lands just after an `$FF`
  terminator, the swap lands on none. Of the six A6-A8 orders over pins
  5, 9, 10, only A6=5, A7=9, A8=10 puts every table entry after a
  terminator and 13 of 14 code targets on instruction starts.
- **Bank layout.** U7's own JP/CALLs point at `$3000-$37FF`, so U7 is
  `$3000-$3FFF`. Patch-window and U7 targets into `$8800-$97FF` hit
  instruction starts 17 of 19 times when U6 sees Z80 A11 unchanged
  (`$8800` = its upper half) and 10 of 19 the other way. Decoded code
  never addresses `$9800-$FFFF`, so those keep the stock A15 mirror.
- **Patch windows.** Each non-blank U5 chunk was matched against every
  8-byte-aligned window of `$0000-$2FFF`. 37 have a unique best match,
  two of them byte-identical to Pac-Man. Three were settled by what the
  replacement does: `$8028` (`CALL $955E`) and `$8090` (`CALL $9561`)
  call routines that load `A` from `$4D2F` or load `DE` and keep `A`,
  which reproduces only the instruction they overwrite at `$2800` and
  `$2780` (`LD A,($4D2F)`, `LD DE,$391D`), not at the other candidates;
  `$81A8` at `$19B8` is `CALL $1000; JR` and the jump lands on the
  original `RST 30` at `$19C4`.

The decoded U5/U6/U7 contents run as the Ms. Pac-Man program in
`play_test`. The latch-set and latch-clear trap addresses have no source yet and
are Unsourced. Covered by `Machine.Aux*` / `Machine.Im2VectorFetchEnablesAuxDecode`
and `web/tests/mspacman.spec.ts`.

## 2. Clocks (never sped up on the live page)

- **Master** 18.432 MHz crystal.
- **CPU** 18.432 / 6 = **3.072 MHz** Z80. `kCpuHz` in `machine.h`.
- **Pixel clock** 18.432 / 3 = 6.144 MHz — 2 pixels per CPU T-state
  (`video.h`'s `kCpuPerLine = kHTotal / 2`).
- **Raster** 384×264 total, ~60.606 Hz (`kCpuPerFrame = 384*264/2 = 50688`
  CPU cycles/frame — asserted directly in `Video.FrameIs50688CpuCycles`).
- **Visible** 288×224 in tube-native (unrotated) space; the real upright
  cabinet's monitor is physically rotated, so the emulator renders
  **224×288** and that's the canvas size `web/index.html` ships (no software
  "rotation setting" — a real Pac-Man cabinet has no such option either).
  The tube is rotated 90 degrees: swap the axes, then mirror the
  result's X, i.e. `upright((kVisH-1) - y, x) = native(x, y)`. A plain
  transpose comes out upside-down against a real ROM set.
- **Vblank IRQ is a held line**, gated by bit 0 of `$5000` (`machine.cpp`'s
  `irq_enable`). Vblank starts at raster line 224 of 264
  (`kVBlankLine = kVisH = 224`). The beam is advanced by the Z80's own
  `Bus::tick`, so the edge lands at the exact T-state; `run_cycles` then
  asserts `/INT` with `Cpu::set_int(true)`, and only a `$5000` write of 0
  drops it (Namco Pac-Man wiki, Memory Mapped I/O: a low write clears /IRQ). A request raised while the
  CPU has interrupts off waits instead of being lost, and an ISR that
  never clears `$5000` re-enters after every `RETI`, as on the board. See §10.
- No visitor-facing turbo, per `CLAUDE.md`. `?test=1` only adds the `__test`
  seam in `app.js`; it does not change `CPU_HZ` or the `requestAnimationFrame`
  pacing.

## 3. Z80 core

Shared core (`retroweb/shared/cpu/cpu_z80.h`/`.cpp`), separate from `retroweb/altair8800/i8080.h`
(8080 stays 8080 — this repo doesn't try to make one core paper over both
ISAs, since Z80 flag polarity and several opcodes genuinely differ). It is
T-state exact: every instruction runs as its UM0080 machine cycles, with
the undocumented set (MEMPTR, Q, block-I/O flags), real IM 0 execution,
16-bit port addresses, and /WAIT, /INT, /NMI and refresh as bus pins.
NMI leaves `iff2` alone so RETN restores the pre-NMI `iff1`
(`Z80.NmiVectorsTo66AndLeavesIff2`). The `EI` shadow is
`Z80.EiDelaysInterruptOneInstruction`. Gates: the named GoogleTests,
Frank Cringle's zexdoc and zexall, and SingleStepTests/z80 (every opcode,
per-T-state bus traces). Design notes and sources are in
`retroweb/shared/cpu/Z80_REVIEW.md`. CI's `z80-test` job runs them; Pac-Man's
native suite then only smokes that this CPU is wired (`Smoke.HwtestBootsSignatureWatchdogAndPaints`).

The self-test ROM only ever programs `IM 1` (RST 7, vector `$0038`); the
real Midway `pacman`/`puckman` program ROM uses `IM 2` instead, reprogramming
the vector byte at runtime via `OUT ($0),A` (0xFA during the self-test's
per-vblank checksum passes, 0xFC once the main game's vblank ISR at `$008D`
takes over — see $233F/$3183 in the Midway pacman disassembly). The board
wires that `OUT`'s data byte straight to a discrete latch feeding the Z80's
interrupt-acknowledge cycle, not a fixed vector; `machine.cpp`'s `irq_vector`
field models that latch, and `Bus::irq_data` returns its current value
instead of a hardcoded byte (`Machine.OutPort0LatchesInterruptVector` is the
board-level regression; `Z80.Im2InterruptReadsVectorTableAtIConcatData` is
the CPU-side one). The 16-vblank watchdog at `$50C0` trips and pulses
/RESET if it is not kicked (`Machine.WatchdogExpiresAfterSixteenVblanksWithoutKick`);
RAM survives that pulse (`Machine.WatchdogResetKeepsRam`).
`$5003` bit 0 is cocktail flip-screen (`Machine.Write5003SetsFlipScreen`,
`Video.FlipScreenFlipsOnlyTheTilemap`).

## 4. Memory map (Midway Pac-Man service manual / schematics)

Sources: Lomont, *Pac-Man Emulation Guide* v0.1 (October 2008), table 1,
and the Namco Pac-Man wiki at obscure.nesdev.org (Memory Map, Memory Mapped I/O).

| Range | Contents |
|---|---|
| `$0000–$3FFF` | Program ROM (16K) |
| `$4000–$43FF` | Video RAM (tile codes, 32×28 visible via `tile_address`'s row/col decode) |
| `$4400–$47FF` | Color RAM (per-tile palette attribute) |
| `$4800–$4BFF` | Nothing. No RAM is fitted (wiki: unused); reads return `$BF`, value Unsourced |
| `$4C00–$4FEF` | Work RAM (1K with the sprite RAM) |
| `$4FF0–$4FFF` | Sprite RAM (8 sprites × 2 bytes: code/flip/attr, color) |
| `$5000` | Interrupt enable (bit 0) |
| `$5001` | Sound enable (bit 0) |
| `$5003` | Flip screen (bit 0) — cocktail-cabinet player-2 upside-down view |
| `$5007` | Coin counter (bit 0) |
| `$5040–$505F` | Namco WSG registers (32 nibbles) |
| `$5060–$506F` | Sprite X/Y coordinate registers |
| `$5000/$5040/$5080/$50C0` (A6-A7) | IN0 / IN1 / DSW1 / DSW2 reads |
| `$50C0–$50FF` (write) | Watchdog reset kick |

A13 isn't decoded above `$4000`, so `$6000–$7FFF` mirror `$4000–$5FFF`
(wiki Memory Map). The I/O page repeats every `$100` (wiki: `$5100–$5FFF`
mirror `$5000–$50FF`), and the 74LS259 latch at `$5000–$5007` repeats every
8 bytes up to `$503F` (wiki Memory Mapped I/O).

`machine.cpp`'s `mem_read`/`mem_write` implement exactly this decode. The
stock PCB leaves Z80 A15 unconnected, so `$8000–$FFFF` mirror
`$0000–$7FFF` (`Machine.PacManA15MirrorsProgramRom`). The aux board is
what makes A15 real extra ROM (see §1). There is no other bank switching.

## 5. Video

8×8 tile playfield (36×28 cells, of which 32×28 are the visible 256×224
score/maze area — the remaining 4 columns are the off-screen "wraparound"
columns used by the tilemap's own row/col scan) plus 8 hardware sprites,
16×16, 2bpp, per the real Namco sprite generator. Both ROMs pack 4 pixels
per byte (not 8 rows of 1bpp per bit-plane): a byte's high nibble is one bit
plane, low nibble the other, one byte per 4-pixel-wide column slice, per
Lomont (Tile ROM and Sprite ROM sections, table 4 and the sprite pixel
ordering grid) — `Video::tile_pixel`/
`sprite_pixel`'s comments spell out the byte/nibble addressing. Verified
against the real `pacman.5e`/`.5f` ROMs by decoding the "NAMCO" credit tiles
and the Pac-Man/ghost sprites into recognizable glyphs; the naive row-major
layout a raw PROM dump might suggest instead decodes to unrecognizable
noise (`Video.TilePixelUsesNibblePackedRom`/`SpritePixelUsesNibblePackedRom`
are the regression tests). Getting the *byte/nibble position* right isn't
enough on its own, though: which nibble supplies pen bit 0 vs. bit 1 also
matters, and monochrome glyphs (letters, digits, walls) set both bits
identically so they look correct either way -- only genuinely 2bpp art
(the maze dots) reveals a swap here, by resolving to a palette index that's
black instead of the intended color. This was a real bring-up bug: dots
were being drawn (confirmed via VRAM byte dumps) but invisible, and the
maze walls came out the wrong hue, both from the same wrong-nibble-to-
pen-bit assignment. Palette is a 2-stage lookup: a 6-bit tile/sprite
"color" attribute plus a 2-bit pixel value index
into a 256-byte lookup PROM, whose output byte indexes a 32-byte RGB color
PROM (`Video::lookup_rgb`) — this two-PROM indirection (not a direct
pixel→RGB table) is genuine Midway hardware, not an emulator simplification.

Sources for the raster, tilemap, sprite and colour paths are the Midway
Pac-Man parts and operating manual (December 1980,
archive.org/details/ArcadeGameManualPac-man): game board schematic
pp.32-33 and the V-RAM addresser A082-91384-B000 schematic p.38.

Counters (p.32). 1H is 8C; 3R/3S count 2H-256H and reload 128 after 511,
so H runs 128-511, 384 states. 3N blanks from H=144 and unblanks at 240,
so the 288 visible pixels are H 240-511 then 128-143. 2R/2S reload 248
after 511 (264 lines); 5M releases /VBLANK at V=16 of the 256-511 half,
so visible line y is V = 272 + y.

Tilemap (`Video::tile_address`, p.38). Three 74LS257s give AB0-AB11 and
take their B side while the counter is 256-511: AB0-AB4 = 8HP-128HP,
AB5-AB9 = 8VP-128VP, plain row-major. In 128-255 the 74LS157s are
selected when 16H = 32H = 64H, which is exactly H 128-143 and 240-255,
the two score columns at each end: AB0-AB4 = 8VP-128VP, AB5 = 8HP,
AB6-AB9 = 64HP, so H 240-255 reads `$3C0-$3FF` and 128-143 reads
`$000-$03F`. AB10 is raw 4H (code, then colour). In the rest of the
blank the 157s pass 4H/16H/32H/64H with AB4-AB11 pulled up, the
`$4FF0` sprite fetch. "P" signals are the counter XOR FLIP (U7-U9), on
8H-128H and 8V-128V only.

Sprites (p.32, `hblank_setup`). The `$5060` registers are a pair of
74LS89s (3F/3H), which read back inverted. 1F/2F add V to the inverted
Y with carry-in low during hblank (5N), 3E (74LS20) flags a match when
the sum's top nibble is all ones and the low nibble is the sprite row,
which works out to row = y - Y + 31 on visible line y. 1E/2E (74LS161)
load the inverted X and step through the 256-cell line buffer at the
pixel clock; the buffer is shown while the H counter is 256-511, so a
sprite wraps modulo 256 and never reaches the score columns. The one
pixel between the loaded cell and the first displayed pixel
(`kLineBufferLag`) depends on load and read pipeline timing that the
scan doesn't resolve; it is Unsourced, chosen so the lowest fully
visible position is (31,16) and the highest (239,256) (Lomont, figure
10). The Y path assumes the fetch and the display use the same V.

Colour (p.33, `lookup_rgb`). 7F (7603) drives red through R18/R17/R16
(1k, 470, 220 from D0-D2), green through R14/R15/R13 (D3-D5) and blue
through R34/R33 (470, 220 from D6-D7) straight into the monitor input.
For a resistor summer into a load R_L, V = V_OH·G_on / (G_total + 1/R_L)
above black, so with full drive scaled to 255 the load and V_OH cancel
and each level is 255·G_on/G_total, rounded: red/green 33, 71, 151 per
bit and blue 81, 174. The table is computed from the resistor values at
compile time. `Video.GunLevelsFollowTheResistorLadder` pins it.

Flip bits: bit 0 of the sprite's first RAM byte is X flip, bit 1 is Y
flip (Lomont, table 6 lists the two flip bits together with the 6-bit
sprite number above them). Both bits apply in *native* (pre-rotation,
landscape) space, before the 90 degree rotation swaps the axes for the upright
cabinet: native Y-flip ends up moving a sprite along the upright screen's
*horizontal* axis (`render`'s `dx` depends on native y) and native X-flip
along its *vertical* axis (`dy = native x`). This axis swap is what made
this bug easy to get only half right during bring-up: an initial fix,
made by watching only on-screen left/right walking and swapping which bit
fed which flip, happened to correct the horizontal case but left the
vertical case (up/down walking) with the mouth opening backwards -- and
independently guessing a fix for *that* by flipping one bit's polarity in
isolation fixed Pac-Man but flipped every ghost upside down (ghosts always
leave both bits 0, so any wrong-axis fix that isn't a no-op at bit=0 will
visibly break them). The bug only fully resolves by using the
bit-to-axis pairing unchanged and letting the existing rotation transform
account for the axis swap, rather than re-deriving new bit semantics by
eye per axis. `Video.SpriteFlipBitsAreXBit0YBit1` and
`Video.UnflippedSpriteKeepsBottomEdgeAtBottom` are the regression tests
(the latter specifically pins the ghosts' always-unflipped case), verified
against real gameplay (walking in all four joystick directions, ghosts
staying dome-up) via a native trace harness before landing.

Sprite transparency isn't "pen index 0 is transparent". Lomont says colour 0
in sprite images is transparent, and that colour is the lookup-PROM output, so
*any* pen whose lookup entry selects colour 0 is clear, not just literal pen 0. The ROM depends on this: it
hides Pac-Man during the "you ate a ghost" freeze not by changing his
sprite code or moving him off-screen, but by pointing him at a color group
whose whole four-pen row resolves to black (confirmed by reading the real
ROM's PROMs: color attribute 0 maps every pen to `#000000`). A renderer
that only skips literal pen 0 still draws that "invisible" Pac-Man as an
opaque black circle, which -- because he's overlapping the eaten ghost's
position and drawn on top of it in sprite priority order -- blots out part
of the "200"/"400"/"800"/"1600" score sprite underneath. This was a real
bring-up bug, reported as "the points are cut off and I see a little
outline of Pac-Man" and reproduced via a native gameplay-simulation
harness that ran the attract mode's Pac-Man-eats-ghosts demo (same sprite
codes and freeze mechanic as real in-game ghost-eating) and dumped the
frame at the moment of the freeze. `Video.SpriteWithAllPensMatchingPenZeroDoesNotOccludeSpriteBehindIt`
is the regression test.

## 6. Sound: Namco WSG

Three voices, each a 20-bit phase accumulator driving a 32-sample lookup
into one of 8 waveforms in a 256-byte wave PROM (`82s126.1m`; the second
sound-region PROM, `82s126.3m`, is not loaded here. Lomont lists both as sound ROMs,
and the 96 kHz clock and 8 selectable waves only use the first 256 nibbles), 4-bit volume
per voice. Sample clock 3.072 MHz / 32 = 96 kHz (`Wsg::advance`'s
`wsg_hz`), resampled to the host's `AudioContext` rate with a zero-order
hold — a simple but audible-artifact-free choice documented inline; a
proper polyphase resampler is not warranted at these sample-rate ratios
(96 kHz down to a typical 48 kHz host rate is only 2:1).

The 32-byte `$5040-$505F` register block is *not* three evenly-spaced,
identically-shaped 5-nibble voice records — that would be the natural
guess, but it's wrong, and it's wrong in a way that doesn't fail loudly:
every individual register write still lands somewhere in the 32-byte
array, so nothing crashes or reads out of bounds, it just corrupts a
different voice's field than the one the game meant to set. The real
board's layout (Lomont, table 7; the Namco Pac-Man wiki, Memory Mapped I/O,
agrees: waveform selects at `$5045`/`$504a`/`$504f`, pitch and volume at
`$5050-$505F`) is:

| Offset | Field |
|---|---|
| `0x05` / `0x0a` / `0x0f` | ch0 / ch1 / ch2 waveform select |
| `0x10` | ch0's low frequency nibble (voices 1 and 2 have no wire to this bit position — their bottom nibble is hardwired 0, a genuine hardware quirk that gives them coarser pitch resolution than voice 0) |
| `0x11-0x14` / `0x15` | ch0 frequency (upper 4 nibbles) / ch0 volume |
| `0x16-0x19` / `0x1a` | ch1 frequency / ch1 volume |
| `0x1b-0x1e` / `0x1f` | ch2 frequency / ch2 volume |

An earlier version of `Wsg::voice_freq`/`voice_wave`/`voice_vol` instead
spaced all three voices 5 nibbles apart starting at offset 0 (frequency at
`v*5`, waveform at `5+v`, volume at `0x15+v`) — plausible-looking, but it
means, e.g., voice 1's frequency LSB nibble (`0x05`) and voice 0's
waveform select alias the *same* register, so an ordinary game write to
one field silently corrupted the other. That produced exactly the bug
report this fix addresses: audio that was present (voices were enabled,
volumes were nonzero, the WASM audio pipeline was working) but sounded
like crackling static — every write to a voice's own register also
scrambled a neighboring voice's unrelated field, so no stable tone ever
held for more than an instruction or two. `Wsg.RegisterMapMatchesRealHardwareOffsets`
and `Wsg.OnlyVoiceZeroHasLowFrequencyNibble` in `shared/namco/tests/wsg_test.cpp`
pin the corrected map; a native harness feeding the real ROM set and
dumping `Machine::audio` to a WAV file (analyzed offline with a sliding
FFT) confirmed the fix produces long, stable single-frequency stretches —
consistent with the siren/waka-waka tones — rather than the broadband,
frame-to-frame-incoherent spectrum a wrong overlapping map produces.

The WSG PROM sample nibble is decoded as an unsigned 4-bit value centred on 8
(`(nibble & 0xf) - 8`; the centring is Unsourced), not a symmetric
±7.5 scale, and the three voices are then summed and normalized like a
resistor-mixed output, not individually clamped per voice.

## 7. Inputs and DIP switches

`IN0`/`IN1` are active-low joystick/start/coin bits exactly as the cabinet's
edge connector presents them (`Inputs` struct's doc comment: "1 = released"
— matches real TTL-input polarity, not an inverted "1 = pressed"
convenience encoding). `web/app.js` maps arrows/WASD to the joystick bits,
`5` / numpad `5` to coin, `1`/`2` and the matching numpad keys to 1P/2P
start. Two on-page 25¢ lamps + coin slots pulse the same IN0 coin bit as
the `5` key — a labelled web-UI stand-in for dropping a quarter, not a
claim that you are dropping a real coin. The coin counter on `$5007`
counts its own pulses (`Machine::coin_counter`, §10).

Factory-typical DIP defaults (1 coin / 1 credit, 3 lives, 10000-point
bonus, normal difficulty, normal ghost names — `Inputs::dsw1 = 0xC9`)
match Lomont's table 10 (service manual not in hand). DSW1 is `$5080`:

| Bits | Function | `0xC9` factory |
|---|---|---|
| 1:0 | Coinage — `01` 1C/1C, `11` 2C/1C, `10` 1C/2C, `00` free | 1C/1C |
| 3:2 | Lives — `00` 1, `01` 2, `10` 3, `11` 5 | 3 |
| 5:4 | Bonus — `00` 10k, `01` 15k, `10` 20k, `11` none | 10k |
| 6 | Difficulty — `1` normal, `0` hard (solder pad on some boards) | normal |
| 7 | Ghost names — `1` normal, `0` alternate (solder pad) | normal |

Rack test is IN0 bit 4 (service-manual SW:7); cabinet upright/cocktail
is an edge-connector jumper on IN1 bit 7. DSW2 (`$50C0`) is unused on
the Midway `pacman` set and stays `0xFF`. The cocktail P2 stick (IN1
bits 0–3) is still unmapped — see §1.

The in-page **DIP switches** panel (`#dipPanel` in `web/index.html`) is
a labelled stand-in for opening the cabinet and flipping that bank, not
a setting on the attract screen. Physical switches survive a power
cycle, so the panel persists in `localStorage` (`retroweb.pacman.dips`,
or `retroweb.mspacman.dips` on the aux-board cabinet) by default —
authentic, not a labelled departure. Ms. Pac-Man has no ghost-names pad
(DSW1 bit 7 is unused and reads 1); that control is hidden on
`?game=mspacman`. The program samples the ports on the next credit;
changing a switch does not reset the board.

GoogleTest pins the port decode (`Machine.Dsw1FactoryDefaultIsThreeLivesBonus10k`,
`Machine.Dsw1PortFollowsInputsIncludingMirrors`). Playwright
`web/tests/dip.spec.ts` drives every control: factory `0xC9`, each
non-default coinage/lives/bonus option, hard difficulty, alternate
ghost names, rack test (IN0 bit 4), cocktail (IN1 bit 7), and a
reload that keeps DSW1 + those two IN bits.

## 8. Test ROM vs. copyrighted ROMs

Ships a from-scratch hardware self-test ROM (`roms/hwtest/gen_hwtest.py`) —
a small Python assembler, not a Pac-Man disassembly. On
a cold boot (and after "Remove ROMs") it runs two short cabinet-style
test patterns — a crosshatch, then color bars, about 1.5 s each — then
holds a help screen in an original 8×8 arcade font (the glyphs are
generated in that script, not copied from `pacman.5e`) explaining that
Namco's program/graphics ROMs are still under copyright and the user has
to load their own Midway `pacman` set; it stays in this browser. On
`?game=mspacman` the same generator emits a second program
(`mspacman_program` / `mspacman-program.bin`) whose help screen says
`MS PAC-MAN ARCADE` / `MS PAC-MAN ROMS` / `MIDWAY MSPACMAN SET` instead
(`Machine.MsPacmanHwtestHelpScreenShowsMsPacManTitle`,
`web/tests/mspacman.spec.ts`). The landing-page cards use two original
286×128 tiles from `retroweb/shared/marquee.py` (`assets/pacman-cabinet.png`
and `assets/mspacman-cabinet.png`), invoked from this same generator. The ROM
also still writes a RAM signature (`TST1` bytes), IRQ-echoes the joystick
into RAM, drives one WSG voice, parks a sprite during the test patterns,
and kicks the watchdog — enough for GoogleTest
(`Smoke.HwtestBootsSignatureWatchdogAndPaints`,
`Machine.HwtestHelpScreenShowsCopyrightPrompt`) and Playwright
(`tests/help.spec.ts`) to assert the help screen without containing a
single byte of Namco's code or graphics. The in-page copy
(`web/index.html`'s `.legal` text) says the same thing.

A real Midway `pacman` or `mspacman` ROM set is opt-in and client-side
only: `web/app.js` accepts a `.zip` or loose chips, maps members by the
standard dump file names (`pacman.6e`, `puckman.6e`, `u5`, `82s123.7f`, …
— the socket ids on the real PCB / aux board), and accepts any dump
whose sizes match the original chips (16K program or four 4K banks, 4K
tiles, 4K sprites, 32-byte color PROM, 256-byte lookup, 256-byte wave;
Ms. Pac-Man also needs U5 2K + U6/U7 4K; overdumps are clipped; the
unused `82s126.3m` timing PROM is ignored). CRC32 is used only to
*label* the generated self-test ROM vs. a user set, not as a whitelist.
Bytes stay in IndexedDB (`retroweb-pacman`, keys `set` / `set-mspacman`)
and are never sent to a server; "Remove ROMs" clears them and reverts
to the test ROM. Extra U5/U6/U7 on the Pac-Man cabinet are ignored
(those sockets are not on the main PCB). A Pac-Man-only zip on
`?game=mspacman` is rejected — that cabinet has the aux board and needs
the three extra chips. This is the same labelled copyright departure as
PC-DOS/IBM BIOS elsewhere in this repo (see `CLAUDE.md`). A failed load
opens `#romErrorHint` (a shared `.site-dialog` in `web/index.html` /
`shared/fullscreen.css`) with the size/name rules above; OK dismisses it.

Two native GoogleTests in `tests/play_test.cpp` are the suite that
actually plays the games. CI has no Namco dump, so both `GTEST_SKIP`.
Locally, drop a dump zip (or the loose chips) in `roms/user/`
(gitignored) — `pacman`/`puckman` for Pac-Man, `mspacman` (6e–6j plus
aux U5/U6/U7) for Ms. Pac-Man — or point `PACMAN_ROM` / `MSPACMAN_ROM`
at that zip or directory, then `make check`. After 8 s of POST/attract
each pulses IN0 coin, asserts credits at `$4E6E` are a 1–9 coin count
(not mid-init garbage), holds 1P start until lives at `$4E14`/`$4E15`
appear or the credit is spent, holds left, and expects the player
sprite at `$4D08`/`$4D09` to move and P1 score at `$4E80` to leave
zero — the same work-RAM cells both programs keep (Data Crystal's
Pac-Man arcade RAM map; Midway disassembly comments at
cubeman.org/arcade-source/mspac.asm). The Ms. Pac-Man test also
asserts the GCC latch starts clear and is set by the first IM2 vector
fetch. A folder that holds both zips picks by aux-board chips, so the
Pac-Man playthrough does not silently run the overlay. The generated
self-test ROM is rejected (`TST1` signature) so a misplaced hwtest dump
cannot pass as a playthrough. These are the regressions that would
have caught the rotation / mouth-facing / ghost-eat / aux-decode
bring-up bugs against a real set; the rest of the suite never inserts
a credit into Namco's program.

## 9. Known simplifications (documented, not silent)

- **No later protection MCUs.** Some Pac-Man bootlegs added extra decoder
  MCUs beyond the GCC PAL/HAL aux board; those stay out of scope. The
  original Ms. Pac-Man conversion kit *is* modeled (see §1). Decrypted
  six-EPROM “no daughterboard” hacks (`mspacmab`) are not a substitute
  for that kit.
- **DIP switches are in-page.** See §7. The panel is a labelled stand-in
  for the cabinet's operator bank; the bit values themselves match the
  real ports. Cocktail-cabinet player-2 joystick (IN1 bits 0–3) is not
  mapped. Covered by `dip.spec.ts` and the two `Machine.Dsw1*` tests.
- **Lockout coil.** The Namco Pac-Man wiki lists `$5006` as connected to
  nothing, so it isn't modeled here either. The `$5007` coin counter is.
- **VRAM wait states (parity P6).** Unverified either way. If the
  Midway sync-bus logic holds /WAIT on CPU access to video RAM during
  active display, the game runs slightly fast here. `Bus::wait` is ready
  for it; it needs the schematic, not a guess.
- **HIGH SCORE RAM is volatile on the real PCB.** There is no battery.
  `$4E88–$4E8A` (BCD TOP, low byte first — Data Crystal RAM map) dies on
  power-off. A page refresh is a power cycle. This page always persists those
  three bytes in IndexedDB (keyed by program CRC) for a user ROM and pokes
  them back once POST is done (irq on after ~8 s — POST's RAM test also
  writes `1` through `$4E00`, and poking `$4E88` during that test fails as
  BAD RAM). Attract is not required: a fast coin+start used to skip the
  restore forever when the gate also demanded `$4E00 == 1`. Prefers the
  saved TOP over leftover attract bytes at `$4E88`, and paints the six
  HIGH SCORE tiles at `$43F2` (Midway `#2ABE` — the ROM only copies TOP to
  the tilemap when a score beats it, not on attract) — a labelled departure.
  **Reset HIGH SCORE** (`#resetHiscore`) is how you get
  the authentic empty table: a `.site-dialog` confirm deletes the save and
  `machine.reset()`s the board. The button is disabled on the self-test ROM.
  `web/tests/hiscore.spec.ts` covers persist-across-reload, Cancel keeping
  the bytes, and confirm clearing them. CI has no Namco dump, so
  save/restore goes through the `?test=1` `__test` seam (CRC-patched
  self-test ROM as a stand-in user set). When a local `pacman.zip` /
  `mspacman.zip` is present, the same file also waits for real attract
  restore (not the seam) and asserts `$4E88` plus the `$43F2` tiles and
  their canvas pixels.

## 10. Parity fixes (2026-10-09)

Items P1-P5, B1-B3 and X5 from `shared/cpu/Z80_ARCADE_PARITY.md`.

- **Held vblank IRQ (P1).** *Fact:* the vblank flip-flop holds /INT until
  `$5000` is written 0. *Why:* the old one-shot `interrupt()` dropped a
  request that arrived with interrupts off. *What:* `set_int` on the edge,
  cleared by `$5000 = 0`. The self-test ROM's ISR now clears and re-arms
  `$5000`, as the real ROM's does. *Source:* Namco Pac-Man wiki, Memory Mapped I/O.
  `Machine.VblankIrqIsHeldUntil5000IsCleared`,
  `Machine.VblankIrqFiresOncePerFrameWhenAcknowledged`,
  `Machine.VblankIrqWaitsOutADisabledCpu`.
- **Sprite clip (P2).** Sprites draw only in native x 16-271, never over
  the two score columns (Lomont: no sprite on the top or bottom two rows).
  `Video.SpritesClipToTheMazeColumns`.
- **Sprite Y (P3).** Every slot uses the Y register minus 31. The
  program copies slots 1-6 from `$4C32` to `$5062` with one `LDIR` at
  `$0181`, and logic sheet A082-91375-A000 does not show a per-slot
  delay. `Video.SlotsWithTheSameYLandOnTheSameLine`.
- **Wraparound (P4).** Each sprite also draws at `sx - 256`.
  `Video.SpritesWrapAt256`.
- **Watchdog 16 (P5).** 74LS161 at 9C counts vblanks, carry resets (Lomont, Watchdog Timer).
- **Raster rendering (B2).** `Video::advance` paints each span as the
  beam crosses it and loads a sprite line buffer in HBLANK, so a mid-frame
  write lands where the beam is. Lower slots win the buffer.
  `Video.MidFrameFlipTakesEffectAtTheBeam`, `Video.LowerSlotWinsTheLineBuffer`.
- **Cocktail flip.** Found on the way. `$5003` flips the tilemap in X and
  Y and leaves sprites alone; the renderer used to flip sprites and never flipped tiles. Lomont,
  Flip Screen. `Video.FlipScreenFlipsOnlyTheTilemap`.
- **Transparency by lookup index.** Compares the lookup entry, not the
  RGB. Same visible result on the real PROMs.
- **Watchdog keeps RAM (B1).** A watchdog trip pulses /RESET; work RAM,
  VRAM and sprite RAM keep their contents. `Machine.WatchdogResetKeepsRam`.
- **Coin counter (B3).** `$5007` D0 rising edges count
  (`Machine.CoinCounterCountsRisingEdges`).

Real `pacman` and `mspacman` sets still boot, take a coin and eat a
pellet (`play_test`), checked against frames dumped from the real ROMs.

## 11. Memory map gaps (2026-10-09)

Found while closing the coverage gaps. `make -C retroweb/pacman coverage`
reports the board code.

- **No RAM at `$4800–$4BFF`.** The board has 1K of work RAM at `$4C00`
  (Namco Pac-Man wiki, Memory Map: `$4800-$4BFF` unused; Lomont's table 1
  shows RAM from `$4800`, the two disagree and the wiki matches the 1K
  chip count). We mapped 2K from `$4800`. Reads there now return `$BF`
  and writes go nowhere. The `$BF` value is Unsourced.
  `Machine.NoRamAt4800TheFloatingBusReadsBF`.
- **Mirrors.** Wiki Memory Map and Memory Mapped I/O: `$6000-$7FFF` mirror
  `$4000-$5FFF`, the I/O page repeats every `$100`, the latch every 8
  bytes, and the ports and watchdog every 64. We had only the A15 mirror.
  `Machine.A13AndA15MirrorTheRamBlocks`,
  `Machine.IoPageIgnoresA8ToA11`, `Machine.WatchdogKicksAnywhereIn50C0To50FF`.
- **Latch bits 2, 4, 5 and 6 do nothing.** The wiki lists them as connected
  to nothing. Lomont calls `$5004-$5006` lamps and a lockout that Pac-Man
  does not use. `Machine.LatchBitsWithNoHardwareDoNothing`.
- **WSG through the bus.** `$5001` and `$5040–$505F` had no test.
  `Machine.SoundChipSitsAt5040AndIsEnabledBy5001`. The WSG tests now drive
  the real `advance()` path, and the test-only `mix_at` mixer is gone.
  `Wsg.AdvancingWithoutOutputStillTurnsThePhase` covers running the chip
  with no host output.

The real `pacman` and `mspacman` sets still boot, take a coin and eat a
pellet.
