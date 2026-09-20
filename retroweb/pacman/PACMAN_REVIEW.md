# Pac-Man arcade board — design notes and open questions

This document plays the same role as `retroweb/altair8800/ALTAIR_REVIEW.md`,
`retroweb/assembler6502/CGOAC6502_REVIEW.md`, and
`retroweb/ibmpc-at/IBM_PCAT_REVIEW.md`: a citation trail for the decisions
behind the emulator's behavior, and an honest list of what isn't built yet.

## 0. Status: v1 of the Midway Pac-Man board is done

The Z80 core (`cpu_z80.{h,cpp}`), video (`video.{h,cpp}`), Namco WSG
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
covers boot of the self-test ROM, the help screen, keyboard/coin-door
input, theme/fullscreen/focus-hint/footer/home chrome, and the ROM loader
(including the rejection dialog). It never ships or fetches Namco's
program. A skippable native playthrough (`Machine.UserRomInsertsCoinStartsAndEatsAPellet`
in `tests/play_test.cpp`) coins in, starts, and eats a pellet against a
local `pacman`/`puckman` dump when one is present; CI skips it. See §8.

Ms. Pac-Man's aux board, a DIP UI, and coin-counter solenoids stay out of
scope (see §1 and §9). Mute, numpad coin/start, and a `.zip` upload path
in Playwright are the remaining coverage gaps, not missing hardware.

## 1. Scope: Midway Pac-Man (1980) single-board upright, no Ms. Pac-Man

Ms. Pac-Man's aux Z80 + ROM-swap decoder, cocktail-cabinet second joystick,
and cabinet bezel art are explicitly out of scope for v1 (see the approved
plan). Only the original Midway/Namco Pac-Man board is modeled.

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
  Every `pacman.cpp` driver in MAME tags the game `ROT90`, defined as
  `FLIP_X | SWAP_XY` (`src/emu/gamedrv.h`): swap the axes, then mirror the
  result's X — i.e. `upright((kVisH-1) - y, x) = native(x, y)`, not the
  "plain" 90°-CCW transpose `Video::render` shipped with initially (which
  came out visibly upside-down against a real ROM set — the two differ by
  a further 180°, not just handedness).
- **IRQ once per vblank**, gated by bit 0 of `$5000` (`machine.cpp`'s
  `irq_enable`). Vblank starts at raster line 224 of 264
  (`kVBlankLine = kVisH = 224`); `Machine::run_cycles` fires `cpu.interrupt()`
  on `video.vblank_edge`, i.e. right at the H-blank/V-blank corner, matching
  "IRQ at the right cycle" rather than a per-CPU-frame poll.
- No visitor-facing turbo, per `CLAUDE.md`. `?test=1` only adds the `__test`
  seam in `app.js`; it does not change `CPU_HZ` or the `requestAnimationFrame`
  pacing.

## 3. Z80 core

Own core (`cpu_z80.h`/`.cpp`), separate from `retroweb/altair8800/i8080.h`
(8080 stays 8080 — this repo doesn't try to make one core paper over both
ISAs, since Z80 flag polarity and several opcodes genuinely differ). Source
for instruction semantics and official T-states: the Zilog Z80 CPU User's
Manual (UM0080). Implemented: documented main/CB/ED/DD/FD opcode maps,
IX/IY (including `(IX+d)`/`(IY+d)` displacement addressing), I/R registers,
`IM 0`/`IM 1`/`IM 2`, NMI (vectors to `$0066`), and the `EI`-delays-interrupt-
by-one-instruction rule (`ei_delay_` in `cpu_z80.h`, tested in
`Z80.EiDelaysInterruptOneInstruction`).

The self-test ROM only ever programs `IM 1` (RST 7, vector `$0038`); the
real Midway `pacman`/`puckman` program ROM uses `IM 2` instead, reprogramming
the vector byte at runtime via `OUT ($0),A` (0xFA during the self-test's
per-vblank checksum passes, 0xFC once the main game's vblank ISR at `$008D`
takes over — see $233F/$3183 in the Midway pacman disassembly). The board
wires that `OUT`'s data byte straight to a discrete latch feeding the Z80's
interrupt-acknowledge cycle, not a fixed vector; `machine.cpp`'s `irq_vector`
field models that latch, and `Bus::irq_data` returns its current value
instead of a hardcoded byte (`Machine.OutPort0LatchesInterruptVector` is the
regression test — ignoring the latch derails the CPU into unmapped memory
within a few frames of loading a real ROM set).

Undocumented `X`/`Y` flag bits (copies of bits 3/5 of the result, or of `A`
memory-refresh timing, on odd ops) are modeled as plain result-bit copies
via `set_szxy`/`set_szxy_p` — the common documented-undocumented-flags
behavior, not the more obscure `MEMPTR`-dependent cases (`SCF`/`CCF`'s flags
sometimes depend on the last-accessed memory address on real silicon).
Pac-Man's own ROM code doesn't depend on those obscure cases; revisit only
if a user-supplied ROM set's behavior ever requires it.

## 4. Memory map (Midway Pac-Man service manual / schematics)

Cross-checked against MAME's `mame/drivers/pacman.cpp` decode only to catch
transcription mistakes — same rule as SIMH vs. real 88-DCDD in `CLAUDE.md`:
MAME's implementation choices aren't themselves a behavior source.

| Range | Contents |
|---|---|
| `$0000–$3FFF` | Program ROM (16K) |
| `$4000–$43FF` | Video RAM (tile codes, 32×28 visible via `vram_offset`'s row/col decode) |
| `$4400–$47FF` | Color RAM (per-tile palette attribute) |
| `$4800–$4FEF` | Work RAM |
| `$4FF0–$4FFF` | Sprite RAM (8 sprites × 2 bytes: code/flip/attr, color) |
| `$5000` | Interrupt enable (bit 0) |
| `$5001` | Sound enable (bit 0) |
| `$5003` | Flip screen (bit 0) — cocktail-cabinet player-2 upside-down view |
| `$5040–$505F` | Namco WSG registers (32 nibbles) |
| `$5060–$506F` | Sprite X/Y coordinate registers |
| `$5000/$5040/$5080/$50C0` (bits `$FFC0`) | IN0 / IN1 / DSW1 / DSW2 reads |
| `$50C0` (write) | Watchdog reset kick |

`machine.cpp`'s `mem_read`/`mem_write` implement exactly this decode. The
board has no bank switching and no protected/paged memory — a flat 16-bit
address space throughout, unlike the 486/AT machines in this repo.

## 5. Video

8×8 tile playfield (36×28 cells, of which 32×28 are the visible 256×224
score/maze area — the remaining 4 columns are the off-screen "wraparound"
columns used by the tilemap's own row/col scan) plus 8 hardware sprites,
16×16, 2bpp, per the real Namco sprite generator. Both ROMs pack 4 pixels
per byte (not 8 rows of 1bpp per bit-plane): a byte's high nibble is one bit
plane, low nibble the other, one byte per 4-pixel-wide column slice, ported
from MAME's `tilelayout`/`spritelayout` `gfx_layout` structs
(`src/mame/pacman/pacman.cpp`, `pengo.cpp`) — `Video::tile_pixel`/
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

`vram_offset`'s row/col-swizzle (`col -= 2; row += 2`, then bit-0x20-gated
either `row + ((col&0x1F)<<5)` or `col + (row<<5)`) reproduces the real
board's non-linear VRAM address decode — the tilemap isn't stored in raster
(row-major) order in VRAM; MAME's `pacman_state::pacman_scan_rows` (a
cross-check, not the source) reflects the same schematic-derived decode.

Sprite coordinate transform (`sy = ram2[i] - 31; sx = 272 - ram2[i+1]`
before flipscreen, `sx = ram2[i+1]; sy = 240 - ram2[i]` after -- note the
register pair's roles swap between the two, an artifact of these being
native pre-rotation coordinates) matches Midway's sprite shifter, cross-
checked against MAME's `draw_sprites`. This was a genuine bring-up bug
(get it wrong and Pac-Man/ghosts still render, just bunched at the wrong
position, e.g. at the ghost house instead of each character's own start
spot) -- `Video.SpritePositionMatchesMameRegisterRoles` is the regression
test, verified against real gameplay (level-start layout) via a native
trace harness before landing.

Flip bits: bit 0 of the sprite's first RAM byte is X flip, bit 1 is Y
flip -- MAME's own `fx = spriteram[offs] & 1` / `fy = spriteram[offs] & 2`
local-variable names (`src/mame/pacman/pacman_v.cpp`), taken literally.
Both bits apply in *native* (pre-rotation, landscape) space, before the
ROT90 (`FLIP_X | SWAP_XY`) transform swaps the axes for the upright
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
visibly break them). The bug only fully resolves by using MAME's literal
bit-to-axis pairing unchanged and letting the existing rotation transform
account for the axis swap, rather than re-deriving new bit semantics by
eye per axis. `Video.SpriteFlipBitsAreXBit0YBit1` and
`Video.UnflippedSpriteKeepsBottomEdgeAtBottom` are the regression tests
(the latter specifically pins the ghosts' always-unflipped case), verified
against real gameplay (walking in all four joystick directions, ghosts
staying dome-up) via a native trace harness before landing.

Sprite transparency isn't "pen index 0 is transparent" -- MAME's
`draw_sprites` resolves it per color group via
`device_palette_interface::transpen_mask(gfx, color, 0)`, which treats
*any* pen whose looked-up RGB matches pen 0's RGB (for that specific color)
as transparent too, not just literal pen 0. The ROM depends on this: it
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
PROM MAME's own `pacman.cpp` ships in the same "namco" ROM region,
`82s126.3m`, is commented `// Timing - not used` in that driver's
`ROM_START(pacman)` and is correctly not loaded here either), 4-bit volume
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
board's layout (MAME `src/devices/sound/namco.cpp`,
`namco_wsg_device::pacman_sound_w`'s own "pacman register map" comment;
independently confirmed by the Midway pacman disassembly's waveform
writes to `$5045`/`$504a`/`$504f` and its 16-byte `$4e8c`->`$5050` `LDIR`
of packed frequency+volume data) is:

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
and `Wsg.OnlyVoiceZeroHasLowFrequencyNibble` in `tests/video_wsg_test.cpp`
pin the corrected map; a native harness feeding the real ROM set and
dumping `Machine::audio` to a WAV file (analyzed offline with a sliding
FFT) confirmed the fix produces long, stable single-frequency stretches —
consistent with the siren/waka-waka tones — rather than the broadband,
frame-to-frame-incoherent spectrum a wrong overlapping map produces.

The WSG PROM sample nibble is decoded as an unsigned 4-bit value biased by
8 (`(nibble & 0xf) - 8`, per MAME's `waveform_r`), not a symmetric
±7.5 scale, and the three voices are then summed and normalized like a
resistor-mixed output, not individually clamped per voice.

## 7. Inputs and DIP switches

`IN0`/`IN1` are active-low joystick/start/coin bits exactly as the cabinet's
edge connector presents them (`Inputs` struct's doc comment: "1 = released"
— matches real TTL-input polarity, not an inverted "1 = pressed"
convenience encoding). `web/app.js` maps arrows/WASD to the joystick bits,
`5` to coin, `1`/`2` to 1P/2P start. Two on-page 25¢ lamps + coin slots
pulse the same IN0 coin bit as the `5` key — a labelled web-UI stand-in
for dropping a quarter, not a claim that the coin-counter/lockout
solenoids are modeled. Factory-typical DIP defaults (3 lives,
10000-point bonus) are hardcoded in `Inputs`'s member
initializers; there is no in-page DIP-switch UI in v1 (real cabinets set
these with physical switches inside the cabinet, not from the attract
screen).

## 8. Test ROM vs. copyrighted ROMs

Ships a from-scratch hardware self-test ROM (`roms/hwtest/gen_hwtest.py`) —
a small Python assembler, not a Pac-Man disassembly or MAME clone ROM. On
a cold boot (and after "Remove ROMs") it runs two short cabinet-style
test patterns — a crosshatch, then color bars, about 1.5 s each — then
holds a help screen in an original 8×8 arcade font (the glyphs are
generated in that script, not copied from `pacman.5e`) explaining that
Namco's program/graphics ROMs are still under copyright and the user has
to load their own Midway `pacman` set; it stays in this browser. The ROM
also still writes a RAM signature (`TST1` bytes), IRQ-echoes the joystick
into RAM, drives one WSG voice, parks a sprite during the test patterns,
and kicks the watchdog — enough for GoogleTest
(`Machine.HwtestHelpScreenShowsCopyrightPrompt`) and Playwright
(`tests/help.spec.ts`) to assert the help screen without containing a
single byte of Namco's code or graphics. The in-page copy
(`web/index.html`'s `.legal` text) says the same thing.

A real Midway `pacman` ROM set is opt-in and client-side only: `web/app.js`
accepts a `.zip` or loose chips, maps members by the usual MAME/board
names (`pacman.6e`, `puckman.6e`, `82s123.7f`, … — the socket ids on the
real PCB), and accepts any dump whose sizes match the original chips
(16K program or four 4K banks, 4K tiles, 4K sprites, 32-byte color PROM,
256-byte lookup, 256-byte wave; overdumps are clipped; the unused
`82s126.3m` timing PROM is ignored). CRC32 is used only to *label* the
generated self-test ROM vs. a user set, not as a whitelist. Bytes stay
in IndexedDB (`retroweb-pacman`) and are never sent to a server; "Remove
ROMs" clears them and reverts to the test ROM. This is the same labelled
copyright departure as PC-DOS/IBM BIOS elsewhere in this repo (see
`CLAUDE.md`). A failed load opens `#romErrorHint` (a shared `.site-dialog`
in `web/index.html` / `shared/fullscreen.css`) with the size/name rules
above; OK dismisses it.

A native GoogleTest, `Machine.UserRomInsertsCoinStartsAndEatsAPellet`
(`tests/play_test.cpp`), is the one suite that actually plays Pac-Man.
CI has no Namco dump, so it `GTEST_SKIP`s. Locally, unzip a MAME
`pacman`/`puckman` set into `roms/user/` (gitignored) or set `PACMAN_ROM`
to that zip or directory, then `make check`. After 8 s of POST/attract it
pulses IN0 coin, asserts credits at `$4E6E` are a 1–9 coin count (not
mid-init garbage), holds 1P start until lives at `$4E14`/`$4E15` appear
or the credit is spent, holds left, and expects the player sprite at
`$4D08`/`$4D09` to move and P1 score at `$4E80` to leave zero — the same
work-RAM cells the original program keeps (Data Crystal's Pac-Man arcade
RAM map; Midway disassembly comments at cubeman.org/arcade-source/mspac.asm).
The generated self-test ROM is rejected here (`TST1` signature) so a
misplaced hwtest dump cannot pass as a playthrough. This is the regression
that would have caught the rotation / mouth-facing / ghost-eat bring-up
bugs against a real set; the rest of the suite never inserts a credit into
Namco's program.

## 9. Known simplifications (documented, not silent)

- **No aux/decoder MCU support.** Some later Pac-Man bootlegs added extra
  protection MCUs; only the standard Midway board is modeled.
- **DIP switches are fixed at factory defaults, no UI.** See §7.
- **Coin counter / lockout solenoid outputs are not modeled.** The on-page
  25¢ slots are a labelled UI control that pulses the same IN0 coin bit as
  the `5` key; they do not drive (or claim to drive) the cabinet's physical
  coin-counter or lockout coils.
