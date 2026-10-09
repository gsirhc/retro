# Z80 and the Z80 arcades -- parity backlog

The open gaps between the shared Z80 core plus the five boards built on it
(Pac-Man, Frogger, Scramble, Galaxian, Galaga) and the real 1979-81 PCBs,
plus test gaps against the repo rules in `CLAUDE.md` and `.claude/arcade.md`.
First audited 2026-10-08 against every source file. MAME items were
checked against current `mamedev/mame` master, not from memory; MAME is
the cross-check, the schematic is the source to cite when an item is
fixed. When an item is fixed, write it up in `Z80_REVIEW.md` (Z80) or the
board's `*_REVIEW.md` (fact, why it matters, what it fixed, source) and
delete it here.

Done: the Z80 core (`Z80_REVIEW.md` §3-§9) and, on 2026-10-09, every
board item that has a source: Pac-Man P1-P5 (`PACMAN_REVIEW.md` §10),
Galaxian-family video V1-V7 and Konami sound K1-K2
(`SCRAMBLE_REVIEW.md` §10), Galaxian discrete sound G1
(`GALAXIAN_REVIEW.md` §9), Galaga GA1-GA5 (`GALAGA_REVIEW.md` §8), and
B1-B3 with X3-X5 on all five. What's left needs a schematic, a chip dump,
or Chris's call.

Rough parity today: **~97%**.

| Area | Parity | What's left |
|------|--------|-------------|
| Z80 core | 100% | none; passes zexall and all 1,604,000 SingleStepTests with bus timing |
| Pac-Man | 98% | VRAM wait states unverified; slot 0-2 offset is MAME-derived |
| Galaxian-family video (shared) | 100% | none known |
| Galaxian (board + discrete sound) | 97% | noise latch clock net (1V vs 2V) |
| Frogger | 99% | cocktail P2 |
| Scramble | 94% | PAL 6J equations |
| Galaga | 95% | 54XX HLE is a fixed burst; MB88 serial rate is MAME's guess |

Already in line with the rules: real 3.072 / 1.789772 MHz clocks, no
visitor turbo, high-score persistence is the sanctioned arcade exception,
every board has smoke + hiscore + DIP + keyboard specs, CI pairs exist.

# TODO

## 1. Needs a schematic or a dump

- **P6. Pac-Man VRAM wait states.** If the Midway sync-bus logic holds
  /WAIT on CPU access to video RAM during active display, the game runs
  slightly fast here. `Bus::wait` is ready. Needs the schematic.
- **P3. Pac-Man sprites 0-2 one line late.** Ported from MAME's
  `m_xoffsethack`, which MAME calls a placement fix. It matches the
  Galaxian line-buffer timing, but the schematic reason is unconfirmed.
- **G2. Galaxian noise latch clock.** MAME labels the D flip-flop clock
  "2V" but generates it at 60*264/2 Hz, which is 1V's rate. We keep the
  rate, derived from the real line timing. Check the net on the schematic.
- **K3. Scramble PAL 6J.** Operations are inferred; MAME also says "Logic
  is not exactly known". Needs a PAL dump.
- **GA5. MB88 serial clock rate.** One shift per instruction cycle is
  MAME's `SERIAL_PRESCALE 6`, marked a guess. Needs the MB8843 datasheet.

## 2. Labelled departures

- **GA4. Galaga 54XX HLE.** Without `54xx.bin` every play command gives a
  fixed ~125 ms noise burst into the real DAC network. The genuine image
  drives all three DACs. A faithful HLE needs the 54XX program's sound
  tables.

## 3. Needs Chris

- **B4. Cocktail P2 controls** are unmapped on all five pages. Wiring
  them needs new visitor-facing key help text, which Chris approves first.

## Not gaps, on purpose

High-score persistence (sanctioned in `.claude/arcade.md`), self-test ROM
in place of copyrighted sets, Ms. Pac-Man aux board, `?test=1` seam.
