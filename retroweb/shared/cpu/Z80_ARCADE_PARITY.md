# Z80 and the Z80 arcades -- parity backlog

The open gaps between the shared Z80 core plus the five boards built on it
(Pac-Man, Frogger, Scramble, Galaxian, Galaga) and the real 1979-81 PCBs,
plus test gaps against the repo rules in `CLAUDE.md` and `.claude/arcade.md`.
Audited against every source file on 2026-10-08. MAME items were checked
against current `mamedev/mame` master, not from memory; MAME is the
cross-check, the schematic is the source to cite when an item is fixed.
When an item is fixed, write it up in `Z80_REVIEW.md` (Z80) or the board's
`*_REVIEW.md` as usual (fact, why it matters, what it fixed, source) and
delete it here. Done so far: the Z80 core, Z1-Z6 with X1-X2
(`Z80_REVIEW.md` §3-§9).

Rough parity today: **~87%**.

| Area | Parity | Biggest gap |
|------|--------|-------------|
| Z80 core | 100% | none open; passes zexall and all 1,604,000 SingleStepTests with bus timing |
| Pac-Man | 90% | vblank IRQ is a one-shot, not a held line; sprite clip/offset/wrap |
| Galaxian-family video (shared) | 76% | sprite priority reversed; no 16-px sprite clip; Galaxian stars don't scroll |
| Galaxian (board + discrete sound) | 70% | discrete sound is a hand-tuned approximation |
| Frogger | 88% | shared video items; AY dry mix |
| Scramble | 82% | shared video items; PAL 6J equations guessed; dry mix |
| Galaga | 80% | starfield speed tied to host refresh rate; sprite transparency rule; 51XX/54XX HLE |

Already in line with the rules: real 3.072 / 1.789772 MHz clocks, no
visitor turbo, high-score persistence is the sanctioned arcade exception,
every board has smoke + hiscore + DIP + keyboard specs, CI pairs exist.

# TODO

## 1. Pac-Man (`pacman/`)

- **P1. Vblank IRQ is lost if interrupts are off.** `run_cycles` calls
  `cpu.interrupt()` once on `vblank_edge`; if IFF1 is clear or the EI delay
  is pending, the request is dropped. The board holds /INT until `$5000`
  is written 0 (MAME `vblank_irq` asserts, `irq_mask_w` clears). Source:
  Midway schematic interrupt flip-flop. The core side is done: drive
  `Cpu::set_int` instead of calling `interrupt()`.
- **P2. Sprites aren't clipped to the maze.** MAME clips sprites to native
  x 16-271 (`spriteclip(2*8, 34*8-1, ...)`); here they draw over the
  score columns. Source: schematic / `pacman_v.cpp`.
- **P3. Sprites 0-2 sit one pixel off.** MAME draws sprite slots 2, 1, 0
  with `sy + m_xoffsethack` (=1 for Pac-Man hardware), the rest without.
  Not modelled. Needs the schematic reason before porting (MAME calls it
  a hack).
- **P4. No sprite wraparound.** MAME also draws each sprite at `sx - 256`.
  Visible as sprites entering the tunnel.
- **P5. Watchdog count.** `kWatchdogFrames = 8`; MAME's `pacman` uses
  `set_vblank_count(16)`. Check the LS161 on the schematic.
  `Machine.WatchdogExpiresAfterEightVblanksWithoutKick` pins 8 today.
- **P6. Possible VRAM wait states.** Unverified. If the Midway sync-bus
  logic stretches CPU access to video RAM during active display, the game
  runs slightly fast here. Needs the schematic before anything changes.
  The core supports it through `Bus::wait`.

## 2. Galaxian-family video (`shared/galaxian/video.cpp`)

Shared by Frogger, Scramble and Galaxian, so each fix lands three times.

- **V1. Sprite priority is reversed.** `pixel_at` walks 7 down to 0 and
  `break`s on the first opaque pixel, so sprite 7 wins. Hardware: the line
  buffer only accepts writes where it holds 0, so lower-numbered sprites
  win (MAME `sprites_draw` comment, renders 7..0 so 0 lands last).
- **V2. No 16-pixel sprite clip.** "16 of the 256 pixels of the sprites
  are hard-clipped at the line buffer ... according to the schematics, the
  first 16" (+1 for the hoffset). Mirrored when flipped X. MAME
  `sprites_clip`.
- **V3. Galaxian stars don't scroll.** The star LFSR is clocked 512*256
  times a frame against a 2^17-1 period; the 6B flip-flops drop two counts
  unflipped. The net effect is a one-step drift per frame (direction
  follows flip X). `backdrop()` uses a fixed origin. Scramble's stars
  genuinely don't scroll (`scramble_draw_stars` uses `y * 512` with no
  origin), so this is Galaxian only. Source: MAME `stars_update_origin`.
- **V4. Galaxian shell colour.** All bullets draw `0xFFFF00`. Shells
  (entries 0-6) are white, the missile (entry 7) is yellow. Scramble's
  all-yellow is right. MAME `galaxian_draw_bullet`.
- **V5. One shell per line.** The hardware draws at most one shell and
  one missile per scanline (last match wins). `bullet_at` draws every
  match. MAME `bullets_draw`.
- **V6. Bullet X position.** MAME draws Galaxian at `255-x-4 .. 255-x-1`
  and Scramble at `255-x-5, 255-x-6`; `bullet_at` lands Galaxian three
  pixels left and Scramble one pixel right of that. Confirm against
  hardware before moving.
- **V7. Screen flip only reaches the tilemap.** Sprites, bullets and the
  star origin ignore `flip_x`/`flip_y`. Cocktail-only, low priority.

## 3. Galaxian board and sound (`galaxian/`)

- **G1. Discrete sound is an approximation.** The LFO is a sine mapped
  `0.4 + bits/15 * 7.6` Hz, the FS tones are FM'd by an invented
  `0.75 + 0.5*lfo`, FIRE sweeps an invented `400 + 2200*env` Hz. Labelled,
  but the furthest thing from hardware in this family. Port the netlist
  (MAME `galaxian_a.cpp` discrete) component by component.

## 4. Konami sound boards (Frogger, Scramble)

- **K1. AY dry mix.** Frogger `$6000` and Scramble `$9000` filter writes
  are ignored. Documented in both reviews, still a gap.
- **K2. AY DAC table.** `kVol` is a pure 3 dB ladder. The real
  AY-3-8910's first steps aren't (MAME uses measured values). Source a
  measurement.
- **K3. Scramble PAL 6J.** Operations are inferred ("equations
  unpublished"). Needs a PAL dump.

## 5. Galaga (`galaga/`)

- **GA1. Starfield speed follows the monitor.** `draw_stars` advances the
  05XX LFSR inside `render()`, which `blit()` calls once per
  `requestAnimationFrame`. A 120 Hz display scrolls stars 2x, 144 Hz 2.4x,
  and a hidden tab freezes them. The 05XX clocks off the pixel clock. Move
  the LFSR into `Video::advance`.
- **GA2. Sprite transparency rule.** `if (pen == 0) continue`. MAME uses
  `transpen_mask(gfx, color, 0x0f)`: any pen whose LUT entry is 0x0F is
  transparent, pen 0 included only because it usually maps there. Same
  lesson as Pac-Man's hidden-during-freeze sprite.
- **GA3. 51XX HLE never spends a credit and ignores coinage.**
  `mcu51_coinage_` is written by command 01 and never read; `note_coins`
  adds one credit per coin edge whatever the DIPs say, and nothing
  decrements on START. Only affects the default (no `51xx.bin`) path.
  Confirm against the 51XX disassembly already cited in the review.
- **GA4. 54XX HLE is a fixed noise burst.** Every play command gives
  ~125 ms of LFSR noise. The LLE path is closer but feeds `o_output`
  straight to the mix with no filter network (MAME `namco54` discrete).
- **GA5. MB88 serial prescaler** not implemented (labelled).

## 6. All boards

- **B1. Watchdog reset wipes RAM.** Every `reset()` zero-fills work RAM
  and VRAM. A watchdog pulses /RESET; RAM keeps its contents. POST
  usually clears it anyway, but a watchdog trip isn't a power cycle.
- **B2. Frame-at-once rendering.** Every renderer reads final RAM state
  once per host frame. No beam timing, so a mid-frame write shows wherever
  the CPU slice happened to stop. The "real graphics adapter ... timing"
  rule wants per-line rendering. Low visible impact on these titles.
  `Bus::tick` lets a board advance the beam mid-instruction.
- **B3. Coin counter / lockout outputs** not modelled (labelled on all five).
- **B4. Cocktail P2 unmapped** (labelled on all five).

## 7. Test parity

- **X3. Shared Galaxian video.** No test pins sprite priority, sprite
  clip, star scroll, shell colour, or one-shell-per-line.
  `Video.GalaxianBulletsAreFourPixels` and
  `Video.YellowShellsDrawAtMatchY` will need updating with V4-V6.
- **X4. Galaga is thin.** 16 native tests. One `Mb88` test for a 431-line
  MCU core. Nothing covers sub/sound CPU lock-step, the sound NMI at lines
  64/192, IRQ hold until the enable is cleared, or 06XX NMI period.
- **X5. Pac-Man.** No IRQ-held test (P1); the watchdog test pins 8 (P5).

## Not gaps, on purpose

High-score persistence (sanctioned in `.claude/arcade.md`), self-test ROM
in place of copyrighted sets, Ms. Pac-Man aux board, `?test=1` seam.
